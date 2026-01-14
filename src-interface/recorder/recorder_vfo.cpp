#include "recorder.h"

#include "main_ui.h"
#include "logger.h"
#include "processing.h"
#include "common/utils.h"
#include "common/ops_state.h"
#include "core/plugin.h"

#include <filesystem>
#include <stdexcept>

namespace satdump
{
    namespace
    {
        bool prepare_live_output_dirs(const std::string &final_dir,
                                      std::string &tmp_dir)
        {
            tmp_dir = ops::build_temp_run_dir(final_dir);
            std::error_code ec;
            if (std::filesystem::exists(tmp_dir, ec))
            {
                std::filesystem::remove_all(tmp_dir, ec);
                if (ec)
                {
                    logger->warn("Failed to clean temp directory %s: %s", tmp_dir.c_str(), ec.message().c_str());
                    return false;
                }
            }
            std::filesystem::create_directories(tmp_dir, ec);
            if (ec)
            {
                logger->error("Failed to create temp directory %s: %s", tmp_dir.c_str(), ec.message().c_str());
                return false;
            }
            return true;
        }

        bool finalize_live_output_dir(const std::string &tmp_dir,
                                      const std::string &final_dir)
        {
            if (tmp_dir.empty() || final_dir.empty() || tmp_dir == final_dir)
                return true;

            std::error_code ec;
            std::filesystem::rename(tmp_dir, final_dir, ec);
            if (ec)
            {
                logger->error("Failed to finalize run directory %s -> %s: %s",
                              tmp_dir.c_str(),
                              final_dir.c_str(),
                              ec.message().c_str());
                return false;
            }
            return true;
        }

        std::string remap_output_path(const std::string &path,
                                      const std::string &from_dir,
                                      const std::string &to_dir)
        {
            if (from_dir.empty() || to_dir.empty())
                return path;
            if (path.rfind(from_dir, 0) != 0)
                return path;
            std::filesystem::path rel = std::filesystem::path(path).lexically_relative(from_dir);
            if (rel.empty())
                return path;
            return (std::filesystem::path(to_dir) / rel).string();
        }
    }

    void RecorderApplication::add_vfo_live(std::string id, std::string name, double freq, satdump::Pipeline vpipeline, nlohmann::json vpipeline_params)
    {
        vfos_mtx.lock();

        try
        {
            VFOInfo wipInfo;
            wipInfo.id = id;
            wipInfo.name = name;
            wipInfo.freq = freq;
            wipInfo.selected_pipeline = vpipeline;
            wipInfo.pipeline_params = vpipeline_params;
            wipInfo.lpool = std::make_shared<ctpl::thread_pool>(8);

            vpipeline_params["samplerate"] = get_samplerate();
            vpipeline_params["baseband_format"] = "cf32";
            vpipeline_params["buffer_size"] = dsp::STREAM_BUFFER_SIZE; // This is required, as we WILL go over the (usually) default 8192 size
            vpipeline_params["start_timestamp"] = (double)time(0);     // Some pipelines need this

            std::string output_dir = prepareAutomatedPipelineFolder(time(0), freq, vpipeline.name, "", false);
            std::string output_dir_tmp;
            if (!prepare_live_output_dirs(output_dir, output_dir_tmp))
                throw std::runtime_error("Failed to prepare live output directory");

            wipInfo.output_dir = output_dir;
            wipInfo.output_dir_tmp = output_dir_tmp;
            wipInfo.run_id = ops::normalize_run_id(std::filesystem::path(output_dir).filename().string());

            wipInfo.live_pipeline = std::make_shared<LivePipeline>(vpipeline, vpipeline_params, output_dir_tmp);
            splitter->add_vfo(id, get_samplerate(), frequency_hz - freq);
            wipInfo.live_pipeline->start(splitter->get_vfo_output(id), *wipInfo.lpool.get());
            splitter->set_vfo_enabled(id, true);

            fft_plot->vfo_freqs.push_back({name, freq});

            vfo_list.push_back(wipInfo);
        }
        catch (std::exception &e)
        {
            logger->error("Error adding VFO : %s", e.what());
        }

        vfos_mtx.unlock();
    }

    void RecorderApplication::add_vfo_reco(std::string id, std::string name, double freq, dsp::BasebandType type, int decimation)
    {
        vfos_mtx.lock();

        try
        {
            VFOInfo wipInfo;
            wipInfo.id = id;
            wipInfo.name = name;
            wipInfo.freq = freq;

            splitter->add_vfo(id, get_samplerate(), frequency_hz - freq);

            if (decimation > 1)
                wipInfo.decim_ptr = std::make_shared<dsp::SmartResamplerBlock<complex_t>>(splitter->get_vfo_output(id), 1, decimation);
            wipInfo.file_sink = std::make_shared<dsp::FileSinkBlock>(decimation > 1 ? wipInfo.decim_ptr->output_stream : splitter->get_vfo_output(id));
            wipInfo.file_sink->set_output_sample_type(type);

            if (decimation > 1)
                wipInfo.decim_ptr->start();
            wipInfo.file_sink->start();

            wipInfo.file_sink->start_recording(config::main_cfg["satdump_directories"]["recording_path"]["value"].get<std::string>() + "/" + prepareBasebandFileName(getTime(), get_samplerate() / decimation, freq), get_samplerate() / decimation);

            splitter->set_vfo_enabled(id, true);

            if (fft)
                fft_plot->vfo_freqs.push_back({name, freq});

            vfo_list.push_back(wipInfo);
        }
        catch (std::exception &e)
        {
            logger->error("Error adding VFO : %s", e.what());
        }

        vfos_mtx.unlock();
    }

    void RecorderApplication::del_vfo(std::string id)
    {
        vfos_mtx.lock();
        auto it = std::find_if(vfo_list.begin(), vfo_list.end(), [&id](VFOInfo &c)
                               { return c.id == id; });
        if (it != vfo_list.end())
        {
            if (fft)
            {
                std::string name = it->name;
                auto it2 = std::find_if(fft_plot->vfo_freqs.begin(), fft_plot->vfo_freqs.end(), [&name](auto &c)
                                        { return c.first == name; });
                if (it2 != fft_plot->vfo_freqs.end())
                    fft_plot->vfo_freqs.erase(it2);
            }

            if (it->file_sink)
                it->file_sink->stop_recording();

            splitter->set_vfo_enabled(it->id, false);

            std::vector<std::string> output_files;
            if (it->selected_pipeline.name != "")
                output_files = it->live_pipeline->getOutputFiles();

            if (it->selected_pipeline.name != "")
                it->live_pipeline->stop();

            if (it->file_sink)
            {
                it->file_sink->stop();
                if (it->decim_ptr)
                    it->decim_ptr->stop();
            }

            splitter->del_vfo(it->id);

            if (it->selected_pipeline.name != "")
            {
                eventBus->fire_event<ops::RunFinalizedEvent>({it->run_id, it->output_dir});
                bool finalized = finalize_live_output_dir(it->output_dir_tmp, it->output_dir);
                std::string output_dir_for_processing = finalized ? it->output_dir : it->output_dir_tmp;

                if (config::main_cfg["user_interface"]["finish_processing_after_live"]["value"].get<bool>() && !output_files.empty())
                {
                    Pipeline pipeline = it->selected_pipeline;
                    std::string input_file = remap_output_path(output_files[0], it->output_dir_tmp, output_dir_for_processing);
                    int start_level = pipeline.live_cfg.normal_live[pipeline.live_cfg.normal_live.size() - 1].first;
                    std::string input_level = pipeline.steps[start_level].level_name;
                    nlohmann::json pipeline_params = it->pipeline_params;
                    ui_thread_pool.push([=](int)
                                        { processing::process(pipeline, input_level, input_file, output_dir_for_processing, pipeline_params); });
                }
            }

            if (it->selected_pipeline.name != "")
            {
                std::string output_dir_for_processing = it->output_dir;
                if (!it->output_dir_tmp.empty() && std::filesystem::exists(it->output_dir_tmp))
                    output_dir_for_processing = it->output_dir_tmp;
                processing::enforce_images_disk_limit(output_dir_for_processing);
            }

            vfo_list.erase(it);
        }
        if (vfo_list.size() == 0)
            if (fft)
                fft_plot->vfo_freqs.clear();
        vfos_mtx.unlock();
    }
}
