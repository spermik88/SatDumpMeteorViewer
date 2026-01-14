#include "status_logger_sink.h"
#include "imgui/imgui_internal.h"
#include "processing.h"
#include "core/config.h"
#include "core/style.h"
#include "common/imgui_utils.h"
#include "main_ui.h"
#include "common/ops_state.h"
#include "common/utils.h"
#include "nlohmann/json_utils.h"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <vector>

SATDUMP_DLL extern float ui_scale;

namespace satdump
{
    namespace
    {
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

        double read_run_timestamp(const std::filesystem::path &run_dir)
        {
            std::filesystem::path meta_path = run_dir / "meta.json";
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

            if (meta.contains("datetime") && meta["datetime"].is_string())
            {
                auto parsed = parse_timestamp(meta["datetime"].get<std::string>());
                if (parsed.has_value())
                    return parsed.value();
            }

            if (meta.contains("time") && meta["time"].is_string())
            {
                auto parsed = parse_timestamp(meta["time"].get<std::string>());
                if (parsed.has_value())
                    return parsed.value();
            }

            return 0.0;
        }
    }
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
        if (!viewer_app)
            return;

        auto mode = viewer_app->getLayerMode();
        ImGui::TextUnformatted("MODE");
        ImGui::SameLine();
        if (ImGui::RadioButton("SINGLE", mode == ViewerApplication::LayerMode::Single))
            viewer_app->setLayerMode(ViewerApplication::LayerMode::Single);
        ImGui::SameLine();
        if (ImGui::RadioButton("STACK", mode == ViewerApplication::LayerMode::Stack))
            viewer_app->setLayerMode(ViewerApplication::LayerMode::Stack);

        ImGui::SameLine();
        bool preview_enabled = viewer_app->isPreviewEnabled();
        bool preview_available = viewer_app->isPreviewAvailable();
        ImGui::BeginDisabled(!preview_available);
        if (ImGui::Checkbox("Preview", &preview_enabled))
            viewer_app->setPreviewEnabled(preview_enabled);
        ImGui::EndDisabled();

        ImGui::SameLine(200 * ui_scale);
        ImGui::TextUnformatted("Layers");
        for (size_t layer_index = 0; layer_index < ViewerApplication::kLayerCount; ++layer_index)
        {
            ImGui::SameLine();
            std::string label = "##layer_" + std::to_string(layer_index + 1);
            bool layer_enabled = viewer_app->isLayerEnabled(layer_index);
            bool layer_available = viewer_app->isLayerAvailable(layer_index);
            ImGui::BeginDisabled(!layer_available);
            if (ImGui::Checkbox(label.c_str(), &layer_enabled))
                viewer_app->setLayerEnabled(layer_index, layer_enabled);
            ImGui::EndDisabled();
        }
        if (mode == ViewerApplication::LayerMode::Stack && viewer_app->shouldWarnAboutStackLayers())
        {
            ImGui::SameLine();
            ImGui::TextColored(style::theme.yellow.Value, "Слишком много слоёв (>3): снижена альфа");
        }
    }

    std::string StatusLoggerSink::resolve_img_time_label()
    {
        ops::OpsStateSnapshot ops_state = ops::get_state();
        if (!selected_run_id.empty())
        {
            if (cached_img_time_run_id != selected_run_id)
            {
                cached_img_time_run_id = selected_run_id;
                double timestamp = 0.0;
                std::filesystem::path run_dir = archive_base_path() / selected_run_id;
                if (std::filesystem::exists(run_dir))
                    timestamp = read_run_timestamp(run_dir);
                if (timestamp > 0.0)
                    cached_img_time_label = timestamp_to_string(timestamp);
                else
                    cached_img_time_label = selected_run_id;
            }
            return cached_img_time_label;
        }

        if (ops_state.pipeline_active && ops_state.live_start_timestamp > 0.0)
            return timestamp_to_string(ops_state.live_start_timestamp);

        return "--";
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
                ops::OpsStateSnapshot ops_state = ops::get_state();
                std::string rx_label = "остановлен";
                ImVec4 rx_color = style::theme.yellow.Value;
                if (ops_state.pipeline_active)
                {
                    if (ops_state.first_valid_frame)
                    {
                        rx_label = "приём";
                        rx_color = style::theme.green.Value;
                    }
                    else
                    {
                        rx_label = "ожидание";
                        rx_color = style::theme.yellow.Value;
                    }
                }
                else if (ops_state.run_finalized)
                {
                    rx_label = "завершено";
                    rx_color = style::theme.green.Value;
                }

                ImGui::TextColored(rx_color, "RX: %s", rx_label.c_str());
                ImGui::SameLine();
                ImGui::Separator();

                const char *sdr_label = "офлайн";
                ImVec4 sdr_color = style::theme.yellow.Value;
                if (recorder_app)
                {
                    auto status = recorder_app->get_source_status();
                    if (status == dsp::DSPSampleSource::SourceStatus::Online)
                    {
                        sdr_label = "онлайн";
                        sdr_color = style::theme.green.Value;
                    }
                    else if (status == dsp::DSPSampleSource::SourceStatus::Error)
                    {
                        sdr_label = "ошибка";
                        sdr_color = style::theme.red.Value;
                    }
                }

                ImGui::TextColored(sdr_color, "SDR: %s", sdr_label);
                ImGui::SameLine();
                ImGui::Separator();
                std::string img_time_label = resolve_img_time_label();
                ImGui::TextDisabled("IMG: %s", img_time_label.c_str());

                float button_width = ImGui::CalcTextSize("Назад").x + (ImGui::GetStyle().FramePadding.x * 2.0f);
                float button_x = ImGui::GetWindowContentRegionMax().x - button_width;
                ImGui::SetCursorPosX(button_x);
                ImGui::BeginDisabled(current_screen == Screen::Viewer);
                if (ImGui::Button("Назад"))
                    current_screen = Screen::Viewer;
                ImGui::EndDisabled();

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
