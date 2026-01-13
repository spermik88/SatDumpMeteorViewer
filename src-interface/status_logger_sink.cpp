#include "status_logger_sink.h"
#include "imgui/imgui_internal.h"
#include "processing.h"
#include "core/config.h"
#include "common/imgui_utils.h"
#include "main_ui.h"

SATDUMP_DLL extern float ui_scale;

namespace satdump
{
    StatusLoggerSink::StatusLoggerSink()
    {
        show_bar = config::main_cfg["user_interface"]["status_bar"]["value"].get<bool>();
        show_log = false;
    }

    StatusLoggerSink::~StatusLoggerSink()
    {
    }

    bool StatusLoggerSink::is_shown()
    {
        return show_bar;
    }

    void StatusLoggerSink::receive(slog::LogMsg log)
    {
        widgets::LoggerSinkWidget::receive(log);
        if (log.lvl >= slog::LOG_INFO)
        {
            if (log.lvl == slog::LOG_INFO)
                lvl = "Info";
            else if (log.lvl == slog::LOG_WARN)
                lvl = "Warning";
            else if (log.lvl == slog::LOG_ERROR)
                lvl = "Error";
            else if (log.lvl == slog::LOG_CRIT)
                lvl = "Critical";
            else
                lvl = "";

            str = log.str;
        }
    }

    void StatusLoggerSink::draw_layer_bar()
    {
        ImGui::TextUnformatted("MODE");
        ImGui::SameLine();
        if (ImGui::RadioButton("SINGLE", layer_mode == 0))
            layer_mode = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("STACK", layer_mode == 1))
            layer_mode = 1;

        ImGui::SameLine(200 * ui_scale);
        ImGui::TextUnformatted("Layers");
        for (size_t layer_index = 0; layer_index < kLayerCount; ++layer_index)
        {
            ImGui::SameLine();
            std::string label = "##layer_" + std::to_string(layer_index + 1);
            ImGui::Checkbox(label.c_str(), &layer_enabled[layer_index]);
        }
    }

    int StatusLoggerSink::draw()
    {
        // Check if status bar should be drawn
        if (!show_bar)
            return 0;

        if (processing::is_processing && ImGuiUtils_OfflineProcessingSelected())
            for (std::shared_ptr<ProcessingModule> module : *processing::ui_call_list)
                if (module->getIDM() == "products_processor")
                    return 0;

        // Draw status bar
        int total_height = 0;
        float row_height = ImGui::GetFrameHeight();
        if (ImGui::BeginViewportSideBar("##MainLayerBar", ImGui::GetMainViewport(), ImGuiDir_Down, row_height,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoNavFocus))
        {
            if (ImGui::BeginMenuBar())
            {
                draw_layer_bar();

                float button_width = ImGui::CalcTextSize("Назад").x + (ImGui::GetStyle().FramePadding.x * 2.0f);
                float button_x = ImGui::GetWindowContentRegionMax().x - button_width;
                ImGui::SetCursorPosX(button_x);
                ImGui::BeginDisabled(current_screen == Screen::Viewer);
                if (ImGui::Button("Назад"))
                    current_screen = Screen::Viewer;
                ImGui::EndDisabled();

                total_height = static_cast<int>(ImGui::GetWindowHeight());
                ImGui::EndMenuBar();
            }
            ImGui::End();
        }

        if (ImGui::BeginViewportSideBar("##MainStatusBar", ImGui::GetMainViewport(), ImGuiDir_Down, row_height,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoNavFocus))
        {
            if (ImGui::BeginMenuBar())
            {
                ImGui::TextUnformatted(lvl.c_str());
                ImGui::SameLine(75 * ui_scale);
                ImGui::Separator();
                ImGui::TextDisabled("%s", str.c_str());
                if (ImGui::IsItemClicked())
                    show_log = true;

                total_height += static_cast<int>(ImGui::GetWindowHeight());
                ImGui::EndMenuBar();
            }
            ImGui::End();
        }

        if (show_log)
        {
            static ImVec2 last_size;
            ImVec2 display_size = ImGui::GetIO().DisplaySize;
            bool did_resize = display_size.x != last_size.x || display_size.y != last_size.y;
            ImGui::SetNextWindowSize(ImVec2(display_size.x, (display_size.y * 0.3) - total_height), did_resize ? ImGuiCond_Always : ImGuiCond_Appearing);
            ImGui::SetNextWindowPos(ImVec2(0, display_size.y * 0.7), did_resize ? ImGuiCond_Always : ImGuiCond_Appearing, ImVec2(0, 0));
            last_size = display_size;

            ImGui::SetNextWindowBgAlpha(1.0);
            ImGui::Begin("SatDump Log", &show_log, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
            widgets::LoggerSinkWidget::draw();

            ImGui::End();
        }

        return total_height;
    }
}
