#define SATDUMP_DLL_EXPORT2 1
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include "processing.h"
#include "logger.h"
#include "core/pipeline.h"

#include "core/config.h"
#include "main_ui.h"
#include "nlohmann/json_utils.h"

namespace satdump
{
    // TMP, MOVE TO HEADER
    extern std::shared_ptr<Application> current_app;
    extern bool in_app;

    std::mutex processing_mutex;

    namespace processing
    {
        namespace
        {
            constexpr uint64_t kImagesLimitBytes = 10ull * 1024ull * 1024ull * 1024ull;

            std::filesystem::path archive_base_path()
            {
                std::filesystem::path preferred = std::filesystem::path("files") / "images";
                if (std::filesystem::exists(preferred))
                    return preferred;
                return std::filesystem::path("images");
            }

            std::optional<double> parse_timestamp(const std::string &value)
            {
                static const std::vector<const char *> formats = {
                    "%Y-%m-%d_%H-%M-%S",
                    "%Y-%m-%d_%H:%M:%S",
                    "%Y-%m-%dT%H:%M:%S",
                    "%Y%m%d_%H%M%S",
                    "%Y%m%d%H%M%S",
                };

                for (const char *format : formats)
                {
                    std::tm tm = {};
                    std::istringstream input(value);
                    input >> std::get_time(&tm, format);
                    if (!input.fail())
                    {
                        std::time_t result = std::mktime(&tm);
                        if (result != -1)
                            return static_cast<double>(result);
                    }
                }

                return std::nullopt;
            }

            double read_meta_timestamp(const std::filesystem::path &meta_path)
            {
                if (!std::filesystem::exists(meta_path))
                    return 0.0;

                nlohmann::ordered_json meta;
                try
                {
                    meta = loadJsonFile(meta_path.string());
                }
                catch (const std::exception &)
                {
                    return 0.0;
                }

                if (meta.contains("timestamp"))
                {
                    if (meta["timestamp"].is_number())
                        return meta["timestamp"].get<double>();
                    if (meta["timestamp"].is_string())
                    {
                        auto parsed = parse_timestamp(meta["timestamp"].get<std::string>());
                        if (parsed.has_value())
                            return parsed.value();
                    }
                }

                return 0.0;
            }

            uint64_t directory_size(const std::filesystem::path &path)
            {
                uint64_t total_size = 0;
                std::error_code ec;
                auto it = std::filesystem::recursive_directory_iterator(
                    path, std::filesystem::directory_options::skip_permission_denied, ec);
                auto end = std::filesystem::recursive_directory_iterator();
                while (it != end)
                {
                    if (it->is_regular_file(ec))
                        total_size += it->file_size(ec);
                    it.increment(ec);
                }
                return total_size;
            }

            bool is_path_within(const std::filesystem::path &base_path, const std::filesystem::path &target_path)
            {
                std::error_code ec;
                auto base_abs = std::filesystem::weakly_canonical(base_path, ec);
                if (ec)
                {
                    ec.clear();
                    base_abs = std::filesystem::absolute(base_path, ec);
                }
                auto target_abs = std::filesystem::weakly_canonical(target_path, ec);
                if (ec)
                {
                    ec.clear();
                    target_abs = std::filesystem::absolute(target_path, ec);
                }

                auto base_it = base_abs.begin();
                auto target_it = target_abs.begin();
                for (; base_it != base_abs.end(); ++base_it, ++target_it)
                {
                    if (target_it == target_abs.end() || *base_it != *target_it)
                        return false;
                }
                return true;
            }

            void remove_run_from_index(const std::filesystem::path &base_path, const std::string &run_id)
            {
                std::filesystem::path index_path = base_path / "index.json";
                if (!std::filesystem::exists(index_path))
                    return;

                auto index = loadJsonFile(index_path.string());
                bool changed = false;
                if (index.is_array())
                {
                    for (auto it = index.begin(); it != index.end();)
                    {
                        bool match = false;
                        if (it->is_string())
                            match = it->get<std::string>() == run_id;
                        else if (it->is_object())
                        {
                            if (it->contains("run_id") && (*it)["run_id"].is_string())
                                match = (*it)["run_id"].get<std::string>() == run_id;
                            else if (it->contains("id") && (*it)["id"].is_string())
                                match = (*it)["id"].get<std::string>() == run_id;
                        }

                        if (match)
                        {
                            it = index.erase(it);
                            changed = true;
                        }
                        else
                        {
                            ++it;
                        }
                    }
                }
                else if (index.is_object())
                {
                    if (index.contains(run_id))
                    {
                        index.erase(run_id);
                        changed = true;
                    }
                }

                if (changed)
                    saveJsonFile(index_path.string(), index);
            }
        }

        void enforce_images_disk_limit(const std::string &output_file)
        {
            std::filesystem::path base_path = archive_base_path();
            if (!std::filesystem::exists(base_path))
                return;

            if (!output_file.empty() && !is_path_within(base_path, std::filesystem::path(output_file)))
                return;

            struct RunEntry
            {
                std::filesystem::path path;
                std::string run_id;
                double timestamp = 0.0;
                uint64_t size = 0;
            };

            std::vector<RunEntry> entries;
            uint64_t total_size = 0;
            for (const auto &entry : std::filesystem::directory_iterator(base_path))
            {
                if (!entry.is_directory())
                    continue;

                RunEntry run_entry;
                run_entry.path = entry.path();
                run_entry.run_id = entry.path().filename().string();
                run_entry.timestamp = read_meta_timestamp(entry.path() / "meta.json");
                run_entry.size = directory_size(entry.path());
                total_size += run_entry.size;
                entries.push_back(run_entry);
            }

            if (total_size <= kImagesLimitBytes)
                return;

            std::sort(entries.begin(), entries.end(),
                      [](const RunEntry &a, const RunEntry &b)
                      {
                          return a.timestamp < b.timestamp;
                      });

            for (const auto &entry : entries)
            {
                if (total_size <= kImagesLimitBytes)
                    break;

                std::error_code ec;
                std::filesystem::remove_all(entry.path, ec);
                if (ec)
                {
                    logger->warn("Failed to remove archive directory %s: %s", entry.path.string().c_str(), ec.message().c_str());
                    continue;
                }
                remove_run_from_index(base_path, entry.run_id);
                if (total_size >= entry.size)
                    total_size -= entry.size;
                else
                    total_size = 0;
            }
        }

        void process(std::string downlink_pipeline,
                     std::string input_level,
                     std::string input_file,
                     std::string output_file,
                     nlohmann::json parameters)
        {
            // Get pipeline
            std::optional<Pipeline> pipeline = getPipelineFromName(downlink_pipeline);
            if (!pipeline.has_value())
            {
                logger->critical("Pipeline " + downlink_pipeline + " does not exist!");
                return;
            }

            process(pipeline.value(), input_level, input_file, output_file, parameters);
        }
        void process(Pipeline downlink_pipeline,
                     std::string input_level,
                     std::string input_file,
                     std::string output_file,
                     nlohmann::json parameters)
        {
            processing_mutex.lock();
            is_processing = true;

            logger->info("Starting processing pipeline " + downlink_pipeline.name + "...");
            logger->debug("Input file (" + input_level + ") : " + input_file);
            logger->debug("Output file : " + output_file);

            if (!std::filesystem::exists(output_file))
                std::filesystem::create_directories(output_file);

            ui_call_list_mutex->lock();
            ui_call_list->clear();
            ui_call_list_mutex->unlock();

            try
            {
                downlink_pipeline.run(input_file, output_file, parameters, input_level, true, ui_call_list, ui_call_list_mutex);
            }
            catch (std::exception &e)
            {
                logger->error("Fatal error running pipeline : " + std::string(e.what()));
                is_processing = false;
                processing_mutex.unlock();
                return;
            }

            is_processing = false;

            logger->info("Done! Goodbye");

            if (config::main_cfg["user_interface"]["open_viewer_post_processing"]["value"].get<bool>())
            {
                if (std::filesystem::exists(output_file + "/dataset.json"))
                {
                    logger->info("Opening viewer!");
                    viewer_app->loadDatasetInViewer(output_file + "/dataset.json");
                }
            }

            enforce_images_disk_limit(output_file);

            processing_mutex.unlock();
        }

        SATDUMP_DLL2 std::shared_ptr<std::vector<std::shared_ptr<ProcessingModule>>> ui_call_list = std::make_shared<std::vector<std::shared_ptr<ProcessingModule>>>();
        SATDUMP_DLL2 std::shared_ptr<std::mutex> ui_call_list_mutex = std::make_shared<std::mutex>();
        SATDUMP_DLL2 bool is_processing = false;
    }
}
