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
    SATDUMP_DLL2 std::shared_ptr<RecorderApplication> recorder_app;
    SATDUMP_DLL2 std::shared_ptr<ViewerApplication> viewer_app;
    std::vector<std::shared_ptr<Application>> other_apps;

    SATDUMP_DLL2 bool update_ui = true;
    SATDUMP_DLL2 Screen current_screen = Screen::Viewer;

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
