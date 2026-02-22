#include "import_export.h"
#include "imgui/imgui.h"
#include "imgui/imgui_stdlib.h"
#include "core/config.h"
#include "core/style.h"
#include "main_ui.h"

#include <fstream>

namespace satdump
{
    TrackingImportExport::TrackingImportExport()
    {
        source_obj = dsp::dsp_sources_registry.begin()->second.getInstance({
                dsp::dsp_sources_registry.begin()->first, "", "" });

        for (auto& this_source : dsp::dsp_sources_registry)
        {
            if (this_source.first == "remote")
                continue;
            sdr_sources.push_back(this_source.first);
            sdr_sources_str += this_source.first + '\0';
        }
        sdr_sources_str += '\0';
        output_directory.setPath(config::main_cfg["satdump_directories"]["live_processing_path"]["value"]);
    }

    bool TrackingImportExport::draw_export()
    {
        bool ret = false;
        if(ImGui::CollapsingHeader("Экспорт в CLI", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGuiStyle& imgui_style = ImGui::GetStyle();
            initial_frequency.draw();
            if (ImGui::Combo("Источник", &selected_sdr, sdr_sources_str.c_str()))
                source_obj = dsp::dsp_sources_registry[sdr_sources[selected_sdr]]
                    .getInstance({sdr_sources[selected_sdr], "", "" });
            ImGui::InputTextWithHint("ID источника", "[Авто]", &source_id);
            source_obj->drawControlUI();
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::InputTextWithHint("HTTP сервер", "[Отключен]", &http_server);
            ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - ImGui::CalcTextSize(u8"\ufc6e Open").x -
                imgui_style.ItemSpacing.x - imgui_style.FramePadding.x * 2);
            output_directory.draw();
            ImGui::SameLine(0, imgui_style.ItemInnerSpacing.x);
            ImGui::TextUnformatted("Выходная папка");
            ImGui::Checkbox("Включить FFT", &fft_enable);
            if (ImGui::Button("Экспортировать конфиг"))
                ret = true;
            export_message.draw();
        }

        return ret;
    }

    bool TrackingImportExport::draw_import()
    {
        bool ret = false;
        if (ImGui::CollapsingHeader("Импорт из CLI"))
        {
            import_file.draw();
            ImGui::Checkbox("Импорт расписания", &import_autotrack_settings);
            ImGui::SameLine();
            ImGui::Checkbox("Импорт настроек ротатора", &import_rotator_settings);
            ImGui::SameLine();
            ImGui::Checkbox("Импорт объектов трекинга", &import_tracked_objects);
            ImGui::Spacing();
            if (ImGui::Button("Импортировать"))
                ret = true;
            import_message.draw();
        }
        return ret;
    }

    void TrackingImportExport::do_export(AutoTrackScheduler &scheduler, ObjectTracker &tracker, std::shared_ptr<rotator::RotatorHandler> rotator_handler)
    {
        // Sanity Checks
        if (!output_directory.isValid())
        {
            export_message.set_message(style::theme.red, "Сначала выберите корректную выходную папку");
            return;
        }
        if (source_obj->get_samplerate() == 0)
        {
            export_message.set_message(style::theme.red, "Сначала задайте корректную частоту дискретизации");
            return;
        }

        // Parameters block
        nlohmann::ordered_json exported_config = {};
        exported_config["parameters"] = source_obj->get_settings();
        exported_config["parameters"]["source"] = sdr_sources[selected_sdr];
        exported_config["parameters"]["samplerate"] = source_obj->get_samplerate();
        exported_config["parameters"]["initial_frequency"] = initial_frequency.get();
        exported_config["parameters"]["fft_enable"] = fft_enable;
        if (!source_id.empty())
            exported_config["parameters"]["source_id"] = source_id;
        if (fft_enable)
        {
            exported_config["parameters"]["fft_size"] = getValueOrDefault(config::main_cfg["user"]["recorder_state"]["fft_size"], 8192);
            exported_config["parameters"]["fft_rate"] = getValueOrDefault(config::main_cfg["user"]["recorder_state"]["fft_rate"], 120);
            exported_config["parameters"]["fft_avgn"] = getValueOrDefault(config::main_cfg["user"]["recorder_state"]["fft_avgn"], 10.0f);
            exported_config["parameters"]["fft_min"] = getValueOrDefault(config::main_cfg["user"]["recorder_state"]["fft_min"], -110.0f);
            exported_config["parameters"]["fft_max"] = getValueOrDefault(config::main_cfg["user"]["recorder_state"]["fft_max"], 0.0f);
        }

        // Loose settings
        exported_config["finish_processing"] = config::main_cfg["user_interface"]["finish_processing_after_live"]["value"];
        exported_config["output_folder"] = output_directory.getPath();
        if (!http_server.empty())
            exported_config["http_server"] = http_server;

        // QTH
        exported_config["qth"] = {};
        exported_config["qth"]["lat"] = config::main_cfg["satdump_general"]["qth_lat"]["value"];
        exported_config["qth"]["lon"] = config::main_cfg["satdump_general"]["qth_lon"]["value"];
        exported_config["qth"]["alt"] = config::main_cfg["satdump_general"]["qth_alt"]["value"];

        // Tracking
        exported_config["tracking"] = {};
        exported_config["tracking"]["rotator_algo"] = tracker.getRotatorConfig();
        exported_config["tracking"]["autotrack_cfg"] = scheduler.getAutoTrackCfg();
        if (rotator_handler)
            exported_config["tracking"]["rotator_config"][rotator_handler->get_id()] = rotator_handler->get_settings();

        // Tracked Objects
        exported_config["tracked_objects"] = scheduler.getTracked();

        // Export
        ui_thread_pool.push([this, exported_config](int) {
            auto result = pfd::save_file("Экспортировать конфиг как...", "", {"JSON files", "*.json"});
            while (!result.ready(1000))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));

            if (result.result().size() > 0)
            {
                std::ofstream test_output(result.result());
                test_output << exported_config.dump(4);
                test_output.close();
                export_message.set_message(style::theme.green, "Конфиг успешно экспортирован");
            }
        });
    }

    void TrackingImportExport::do_import(AutoTrackScheduler &scheduler, ObjectTracker &tracker, std::shared_ptr<rotator::RotatorHandler> rotator_handler)
    {
        // Sanity Checks
        if (!import_file.isValid())
        {
            import_message.set_message(style::theme.red, "Сначала выберите корректный файл импорта");
            return;
        }
        if (!import_autotrack_settings && !import_tracked_objects && !import_rotator_settings)
        {
            import_message.set_message(style::theme.red, "Выберите хотя бы один пункт для импорта");
            return;
        }

        try
        {
            nlohmann::ordered_json imported_config = loadJsonFile(import_file.getPath());
            if (import_autotrack_settings)
                scheduler.setAutoTrackCfg(imported_config["tracking"]["autotrack_cfg"]);
            if(import_tracked_objects)
                scheduler.setTracked(imported_config["tracked_objects"]);
            if (import_rotator_settings)
            {
                tracker.setRotatorConfig(imported_config["tracking"]["rotator_algo"]);
                if (imported_config["tracking"]["rotator_config"].contains(rotator_handler->get_id()))
                {
                    if (rotator_handler->is_connected())
                        logger->warn("Not importing some rotator settings as the rotator is currently connected");
                    else
                        rotator_handler->set_settings(imported_config["tracking"]["rotator_config"][rotator_handler->get_id()]);
                }
                else
                    logger->warn("Not importing some rotator settings as they are not included in this config");
            }
        }
        catch (std::exception &e)
        {
            import_message.set_message(style::theme.red, "Ошибка импорта конфига!");
            logger->error("Tracking import error: %s", e.what());
            return;
        }

        import_message.set_message(style::theme.green, "Конфиг успешно импортирован");
    }
}
