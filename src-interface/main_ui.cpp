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
#include <filesystem>
#include <mutex>

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
        constexpr bool kApplianceMode =
#ifdef __ANDROID__
            true;
#else
            false;
#endif

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
        std::mutex archive_index_mutex;

        std::filesystem::path archive_base_path()
        {
            std::filesystem::path preferred = std::filesystem::path("files") / "images";
            if (std::filesystem::exists(preferred))
                return preferred;
            return std::filesystem::path("images");
        }

        bool parse_archive_meta(const nlohmann::ordered_json &meta,
                                const std::filesystem::path &run_dir,
                                ArchiveEntry &item)
        {
            if (!meta.is_object())
                return false;
            if (!meta.contains("run_id") || !meta["run_id"].is_string())
                return false;
            if (!meta.contains("timestamp") || !meta["timestamp"].is_number())
                return false;
            if (!meta.contains("datetime_local") || !meta["datetime_local"].is_string())
                return false;
            if (!meta.contains("preview") || !meta["preview"].is_string())
                return false;
            if (!meta.contains("source") || !meta["source"].is_string())
                return false;
            if (meta["source"].get<std::string>() != "meteor_lrpt")
                return false;
            if (!meta.contains("layers") || !meta["layers"].is_array())
                return false;
            if (!meta.contains("default_layer") || !meta["default_layer"].is_number_integer())
                return false;
            if (!meta.contains("has_composite") || !meta["has_composite"].is_boolean())
                return false;

            std::string run_id = meta["run_id"].get<std::string>();
            if (run_id.empty() || run_id != run_dir.filename().string())
                return false;

            auto layers = meta["layers"].get<std::vector<std::string>>();
            if (layers.empty() || layers.size() > 6)
                return false;
            for (const auto &layer_name : layers)
            {
                if (layer_name.empty())
                    return false;
                if (!std::filesystem::exists(run_dir / layer_name))
                    return false;
            }

            int default_layer = meta["default_layer"].get<int>();
            if (default_layer < 1 || default_layer > static_cast<int>(layers.size()))
                return false;

            std::filesystem::path preview_path = run_dir / meta["preview"].get<std::string>();
            if (!std::filesystem::exists(preview_path))
                return false;

            bool has_composite = meta["has_composite"].get<bool>();
            if (has_composite && !std::filesystem::exists(run_dir / "composite.png"))
                return false;

            std::filesystem::path dataset_path = run_dir / "dataset.json";
            if (!std::filesystem::exists(dataset_path))
                return false;

            item.run_id = run_id;
            item.directory_path = run_dir.string();
            item.dataset_path = dataset_path.string();
            item.thumb_path = (run_dir / "thumb.png").string();
            item.timestamp = meta["timestamp"].get<double>();
            item.label = meta["datetime_local"].get<std::string>();
            if (item.label.empty() && item.timestamp > 0.0)
                item.label = timestamp_to_string(item.timestamp);
            return true;
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
            std::lock_guard<std::mutex> lock(archive_index_mutex);
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

                if (!parse_archive_meta(meta, entry.path(), item))
                    continue;

                generate_thumbnail_if_needed(entry.path());

                archive_entries.push_back(item);
            }

            std::sort(archive_entries.begin(), archive_entries.end(),
                      [](const ArchiveEntry &a, const ArchiveEntry &b)
                      {
                          return a.timestamp > b.timestamp;
                      });
        }

        const ArchiveEntry *find_archive_entry(const std::string &run_id)
        {
            for (const auto &entry : archive_entries)
                if (entry.run_id == run_id)
                    return &entry;
            return nullptr;
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

    bool is_appliance_mode()
    {
        return kApplianceMode;
    }

    void invalidate_archive_index()
    {
        std::lock_guard<std::mutex> lock(archive_index_mutex);
        archive_index_ready = false;
    }

    std::vector<std::string> get_archive_run_ids()
    {
        if (!archive_index_ready)
            load_archive_index();

        std::vector<std::string> out;
        out.reserve(archive_entries.size());
        for (const auto &entry : archive_entries)
            out.push_back(entry.run_id);
        return out;
    }

    bool open_run_in_viewer(const std::string &run_id)
    {
        if (run_id.empty())
            return false;

        if (!archive_index_ready)
            load_archive_index();

        const ArchiveEntry *entry = find_archive_entry(run_id);
        if (!entry)
            return false;

        selected_run_id = run_id;
        if (std::filesystem::exists(entry->dataset_path))
            viewer_app->loadDatasetInViewer(entry->dataset_path);
        current_screen = Screen::Viewer;
        return true;
    }

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

        eventBus->register_handler<ops::FirstValidFrameEvent>([](const ops::FirstValidFrameEvent &evt)
                                                              {
                                                                  selected_run_id = ops::normalize_run_id(evt.run_id);
                                                                  current_screen = Screen::Viewer;

                                                                  ops::OpsStateSnapshot state = ops::get_state();
                                                                  std::filesystem::path dataset_path = std::filesystem::path(state.current_run_tmp_dir) / "dataset.json";
                                                                  if (!std::filesystem::exists(dataset_path))
                                                                      dataset_path = std::filesystem::path(state.current_run_final_dir) / "dataset.json";
                                                                  if (std::filesystem::exists(dataset_path))
                                                                  {
                                                                      ui_thread_pool.push([dataset_path](int)
                                                                                          { viewer_app->loadDatasetInViewer(dataset_path.string()); });
                                                                  }
                                                              });
        eventBus->register_handler<ops::RunFinalizedEvent>([](const ops::RunFinalizedEvent &)
                                                           { invalidate_archive_index(); });
        eventBus->register_handler<ops::FifoDeleteEvent>([](const ops::FifoDeleteEvent &evt)
                                                         {
                                                             invalidate_archive_index();
                                                             if (selected_run_id == evt.run_id)
                                                             {
                                                                 std::vector<std::string> runs = get_archive_run_ids();
                                                                 if (runs.empty())
                                                                 {
                                                                     selected_run_id.clear();
                                                                     current_screen = Screen::Archive;
                                                                 }
                                                                 else
                                                                     open_run_in_viewer(runs.front());
                                                             }
                                                         });
        eventBus->register_handler<ops::ArchiveChangedEvent>([](const ops::ArchiveChangedEvent &)
                                                             { invalidate_archive_index(); });

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
        if (recorder_app)
            recorder_app->tick_background();

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
                ImGui::TextUnformatted("Архив");
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
                                    open_run_in_viewer(entry.run_id);
                            }
                            else
                            {
                                if (ImGui::Button("Нет\nминиатюры", ImVec2(max_image, max_image)))
                                    open_run_in_viewer(entry.run_id);
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
                if (ImGui::Button("Получить тайл с сервера"))
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
