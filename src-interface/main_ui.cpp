#define SATDUMP_DLL_EXPORT2 1
#include "main_ui.h"
#include "imgui/imgui_flags.h"
#include "imgui/imgui.h"
#include "settings.h"
#include "satdump_vars.h"
#include "core/backend.h"
#include "common/audio/audio_sink.h"
#include "imgui_notify/imgui_notify.h"
#include "notify_logger_sink.h"
#include "status_logger_sink.h"
#include "imgui/imgui_image.h"
#include "common/image/io.h"
#include "common/image/image.h"
#include "common/utils.h"
#include "nlohmann/json_utils.h"
#include "common/ops_state.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>

#include "imgui/implot/implot.h"
#include "imgui/implot3d/implot3d.h"

// #define ENABLE_DEBUG_MAP
#ifdef ENABLE_DEBUG_MAP
#include "common/widgets/image_view.h"
float lat = 0, lon = 0, lat1 = 0, lon1 = 0;
int zoom = 0;
image::Image<uint8_t> img(800, 400, 3);
ImageViewWidget ivw;
#endif

namespace satdump
{
    namespace
    {
        struct ArchiveEntry
        {
            std::string run_id;
            std::string label;
            std::string directory_path;
            std::string dataset_path;
            std::string thumb_path;
            double timestamp = 0.0;
            unsigned int texture_id = 0;
            int texture_width = 0;
            int texture_height = 0;
        };

        std::vector<ArchiveEntry> archive_entries;
        bool archive_index_ready = false;

        std::filesystem::path archive_base_path()
        {
            std::filesystem::path preferred = std::filesystem::path("files") / "images";
            if (std::filesystem::exists(preferred))
                return preferred;
            return std::filesystem::path("images");
        }

        double file_time_to_timestamp(const std::filesystem::file_time_type &ftime)
        {
            auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            return static_cast<double>(std::chrono::system_clock::to_time_t(system_time));
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

        void generate_thumbnail_if_needed(const std::filesystem::path &dir_path)
        {
            std::filesystem::path thumb_path = dir_path / "thumb.png";
            if (std::filesystem::exists(thumb_path))
                return;

            std::filesystem::path preview_path = dir_path / "preview.png";
            if (!std::filesystem::exists(preview_path))
                return;

            image::Image preview;
            image::load_png(preview, preview_path.string());
            if (preview.width() == 0 || preview.height() == 0)
                return;

            const int max_size = 256;
            size_t width = preview.width();
            size_t height = preview.height();
            size_t max_dim = std::max(width, height);
            if (max_dim > static_cast<size_t>(max_size))
            {
                double scale = static_cast<double>(max_size) / static_cast<double>(max_dim);
                int new_width = std::max(1, static_cast<int>(width * scale));
                int new_height = std::max(1, static_cast<int>(height * scale));
                preview.resize_bilinear(new_width, new_height, false);
            }

            image::save_img(preview, thumb_path.string());
        }

        void load_archive_index()
        {
            archive_entries.clear();
            archive_index_ready = true;
            std::filesystem::path base_path = archive_base_path();

            if (!std::filesystem::exists(base_path))
                return;

            for (const auto &entry : std::filesystem::directory_iterator(base_path))
            {
                if (!entry.is_directory())
                    continue;
                if (ops::is_temp_run_dir(entry.path().filename().string()))
                    continue;

                ArchiveEntry item;
                item.run_id = entry.path().filename().string();
                item.directory_path = entry.path().string();
                item.dataset_path = (entry.path() / "dataset.json").string();
                item.thumb_path = (entry.path() / "thumb.png").string();

                std::filesystem::path meta_path = entry.path() / "meta.json";
                if (!std::filesystem::exists(meta_path))
                    continue;

                nlohmann::ordered_json meta;
                try
                {
                    meta = loadJsonFile(meta_path.string());
                }
                catch (const std::exception &)
                {
                    continue;
                }

                if (meta.contains("timestamp"))
                {
                    if (meta["timestamp"].is_number())
                    {
                        item.timestamp = meta["timestamp"].get<double>();
                        item.label = timestamp_to_string(item.timestamp);
                    }
                    else if (meta["timestamp"].is_string())
                    {
                        item.label = meta["timestamp"].get<std::string>();
                        auto parsed = parse_timestamp(item.label);
                        if (parsed.has_value())
                            item.timestamp = parsed.value();
                    }
                }

                if (item.label.empty())
                {
                    if (meta.contains("datetime") && meta["datetime"].is_string())
                        item.label = meta["datetime"].get<std::string>();
                    else if (meta.contains("time") && meta["time"].is_string())
                        item.label = meta["time"].get<std::string>();
                }

                if (item.timestamp == 0.0)
                {
                    auto parsed = parse_timestamp(item.run_id);
                    if (parsed.has_value())
                        item.timestamp = parsed.value();
                }

                if (item.timestamp == 0.0)
                    item.timestamp = file_time_to_timestamp(entry.last_write_time());

                if (item.label.empty())
                {
                    if (item.timestamp > 0.0)
                        item.label = timestamp_to_string(item.timestamp);
                    else
                        item.label = item.run_id;
                }

                generate_thumbnail_if_needed(entry.path());

                archive_entries.push_back(item);
            }

            std::sort(archive_entries.begin(), archive_entries.end(),
                      [](const ArchiveEntry &a, const ArchiveEntry &b)
                      {
                          return a.timestamp > b.timestamp;
                      });
        }

        bool ensure_thumbnail_texture(ArchiveEntry &entry)
        {
            if (entry.texture_id != 0)
                return true;

            if (!std::filesystem::exists(entry.thumb_path))
                return false;

            image::Image thumb;
            image::load_png(thumb, entry.thumb_path);
            if (thumb.width() == 0 || thumb.height() == 0)
                return false;

            if (thumb.depth() != 8)
                thumb = thumb.to_depth(8);
            thumb.to_rgba();

            std::vector<uint32_t> buffer(thumb.width() * thumb.height());
            image::image_to_rgba(thumb, buffer.data());

            entry.texture_id = makeImageTexture();
            updateImageTexture(entry.texture_id, buffer.data(), static_cast<int>(thumb.width()), static_cast<int>(thumb.height()));
            entry.texture_width = static_cast<int>(thumb.width());
            entry.texture_height = static_cast<int>(thumb.height());
            return true;
        }
    }

    SATDUMP_DLL2 std::shared_ptr<RecorderApplication> recorder_app;
    SATDUMP_DLL2 std::shared_ptr<ViewerApplication> viewer_app;
    std::vector<std::shared_ptr<Application>> other_apps;

    SATDUMP_DLL2 bool update_ui = true;
    SATDUMP_DLL2 Screen current_screen = Screen::Viewer;
    SATDUMP_DLL2 std::string selected_run_id;

    std::shared_ptr<NotifyLoggerSink> notify_logger_sink;
    std::shared_ptr<StatusLoggerSink> status_logger_sink;

    void initMainUI()
    {
        ImPlot::CreateContext();
        ImPlot3D::CreateContext();

        audio::registerSinks();
        settings::setup();

        registerViewerHandlers();

        recorder_app = std::make_shared<RecorderApplication>();
        viewer_app = std::make_shared<ViewerApplication>();

        eventBus->fire_event<AddGUIApplicationEvent>({other_apps});

        // Logger status bar sync
        status_logger_sink = std::make_shared<StatusLoggerSink>();
        if (status_logger_sink->is_shown())
            logger->add_sink(status_logger_sink);

        // Shut down the logger init buffer manually to prevent init warnings
        // From showing as a toast, or in the product processor screen
        completeLoggerInit();

        // Logger notify sink
        notify_logger_sink = std::make_shared<NotifyLoggerSink>();
        logger->add_sink(notify_logger_sink);

        load_archive_index();
    }

    void exitMainUI()
    {
        recorder_app->save_settings();
        viewer_app->save_settings();
        config::saveUserConfig();
        recorder_app.reset();
        viewer_app.reset();
    }

    void renderMainUI()
    {
        if (update_ui)
        {
            style::setStyle();
            style::setFonts(ui_scale);
            update_ui = false;
        }

        std::pair<int, int> dims = backend::beginFrame();
        dims.second -= status_logger_sink->draw();

        // else
        {
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({(float)dims.first, (float)dims.second});
            ImGui::Begin("SatDump UI", nullptr, NOWINDOW_FLAGS | ImGuiWindowFlags_NoDecoration);
            if (current_screen == Screen::Viewer)
            {
                viewer_app->draw();
            }
            else
            {
                ImGui::BeginChild("archive_screen", ImGui::GetContentRegionAvail());
                ImGui::TextUnformatted("Archive");
                ImGui::Separator();

                if (!archive_index_ready)
                    load_archive_index();

                if (archive_entries.empty())
                {
                    ImGui::TextUnformatted("Нет данных в архиве.");
                }
                else
                {
                    ImVec2 available = ImGui::GetContentRegionAvail();
                    float tile_size = 180.0f * ui_scale;
                    float spacing = ImGui::GetStyle().ItemSpacing.x;
                    int columns = std::max(1, static_cast<int>((available.x + spacing) / (tile_size + spacing)));

                    if (ImGui::BeginTable("archive_grid", columns, ImGuiTableFlags_SizingFixedFit))
                    {
                        for (auto &entry : archive_entries)
                        {
                            ImGui::TableNextColumn();
                            ImGui::PushID(entry.run_id.c_str());
                            ImGui::BeginGroup();

                            float max_image = 150.0f * ui_scale;
                            bool has_texture = ensure_thumbnail_texture(entry);
                            if (has_texture)
                            {
                                float aspect = entry.texture_height > 0 ? (static_cast<float>(entry.texture_width) / static_cast<float>(entry.texture_height)) : 1.0f;
                                float draw_w = max_image;
                                float draw_h = max_image;
                                if (aspect >= 1.0f)
                                    draw_h = max_image / aspect;
                                else
                                    draw_w = max_image * aspect;

                                ImVec2 cursor = ImGui::GetCursorPos();
                                ImGui::SetCursorPosX(cursor.x + (max_image - draw_w) * 0.5f);
                                if (ImGui::ImageButton((void *)(intptr_t)entry.texture_id, ImVec2(draw_w, draw_h)))
                                {
                                    selected_run_id = entry.run_id;
                                    if (std::filesystem::exists(entry.dataset_path))
                                        viewer_app->loadDatasetInViewer(entry.dataset_path);
                                    current_screen = Screen::Viewer;
                                }
                            }
                            else
                            {
                                if (ImGui::Button("Нет\nминиатюры", ImVec2(max_image, max_image)))
                                {
                                    selected_run_id = entry.run_id;
                                    if (std::filesystem::exists(entry.dataset_path))
                                        viewer_app->loadDatasetInViewer(entry.dataset_path);
                                    current_screen = Screen::Viewer;
                                }
                            }

                            ImGui::TextWrapped("%s", entry.label.c_str());
                            ImGui::EndGroup();
                            ImGui::PopID();
                        }

                        ImGui::EndTable();
                    }
                }
                ImGui::EndChild();
            }
#ifdef ENABLE_DEBUG_MAP
                tileMap tm;
                ImGui::SetNextItemWidth(120);
                ImGui::InputFloat("Latitude", &lat);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(120);
                ImGui::InputFloat("Longitude", &lon);
                ImGui::SetNextItemWidth(120);
                ImGui::InputFloat("Latitude##1", &lat1);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(120);
                ImGui::InputFloat("Longitude##1", &lon1);
                ImGui::SetNextItemWidth(250);
                ImGui::SliderInt("Zoom", &zoom, 0, 19);
                if (ImGui::Button("Get tile from server"))
                {
                    // mapTile tl(tm.downloadTile(tm.coorToTile({lat, lon}, zoom), zoom));
                    img = tm.getMapImage({lat, lon}, {lat1, lon1}, zoom);
                    ivw.update(img);
                }
                ivw.draw(ImVec2(800, 400));
#endif
            ImGuiUtils_SendCurrentWindowToBack();
            ImGui::End();

            if (settings::show_imgui_demo)
            {
                ImGui::ShowDemoWindow();
                ImPlot::ShowDemoWindow();
                ImPlot3D::ShowDemoWindow();
            }
        }

        // Render toasts on top of everything, at the end of your code!
        // You should push style vars here
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 5.f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, (ImVec4)style::theme.notification_bg);
        notify_logger_sink->notify_mutex.lock();
        ImGui::RenderNotifications();
        notify_logger_sink->notify_mutex.unlock();
        ImGui::PopStyleVar(1);
        ImGui::PopStyleColor(1);

        backend::endFrame();
    }

    SATDUMP_DLL2 ctpl::thread_pool ui_thread_pool(8);
}
