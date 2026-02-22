#include "viewer.h"
#include "imgui/imgui_stdlib.h"
#include "common/map/map_drawer.h"
#include "common/projection/reprojector.h"
#include "logger.h"
#include "resources.h"
#include "core/style.h"
#include "main_ui.h"
#include "common/image/image_utils.h"
#include "common/widgets/switch.h"
#include "common/widgets/stepped_slider.h"
#include "imgui/pfd/pfd_utils.h"

#include "common/image/meta.h"
#include "common/projection/projs2/proj_json.h"
#include "common/widgets/spinner.h"

#include "settings.h"
#include "common/widgets/json_editor.h"

namespace satdump
{
    int osm_url_regex_len = 0;
    float general_progress = 0;
    float general_sum = 1;
    float *progress_pointer = nullptr;

    void ViewerApplication::drawProjectionPanel()
    {
        bool disable_buttons = projections_are_generating || projections_loading_new_layer;
        int active_layers = 0;
        projection_layers_mtx.lock();
        for (auto &lay : projection_layers)
            if (lay.enabled)
                active_layers++;
        projection_layers_mtx.unlock();
        
        if (ImGui::CollapsingHeader("Проекция", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Выходное изображение:");
            ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * 0.40f);
            ImGui::InputInt("##width", &projections_image_width, 0);
            ImGui::SameLine();
            ImGui::Text(u8"\uea76");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * 0.40f);
            ImGui::InputInt("##height", &projections_image_height, 0);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * 0.96f);
            ImGui::Combo("##targetproj", &projections_current_selected_proj, "Эквидистантная цилиндрическая\0"
                                                                             "UTM (Меркатор)\0"
                                                                             "Стереографическая\0"
                                                                             "Спутниковая (TPERS)\0"
                         //  "Azimuthal Equidistant\0"
            );

            if (projections_current_selected_proj == 0)
            {
                if (!projection_auto_mode)
                {
                    ImGui::Text("Координаты верхнего левого угла:");
                    ImGui::InputFloat("Lat##tl", &projections_equirectangular_tl_lat);
                    ImGui::InputFloat("Lon##tl", &projections_equirectangular_tl_lon);
                    ImGui::Spacing();
                    ImGui::Text("Координаты нижнего правого угла:");
                    ImGui::InputFloat("Lat##br", &projections_equirectangular_br_lat);
                    ImGui::InputFloat("Lon##br", &projections_equirectangular_br_lon);
                }
            }
            else if (projections_current_selected_proj == 1)
            {
                ImGui::InputInt("Зона UTM###projutmzone", &projections_utm_zone);
                if (projections_utm_zone > 60)
                    projections_utm_zone = 60;
                if (projections_utm_zone < 1)
                    projections_utm_zone = 1;
                ImGui::Checkbox("Южное полушарие###projutmsouth", &projections_utm_south);
                ImGui::InputFloat("Смещение по северингу (м)##utm", &projections_utm_offset_y);
                ImGui::Spacing();
                ImGui::InputFloat("Масштаб (м/пикс)##utm", &projections_utm_scale);
            }
            else if (projections_current_selected_proj == 2)
            {
                ImGui::Text("Координаты центра:");
                ImGui::InputFloat("Lat##stereo", &projections_stereo_center_lat);
                ImGui::InputFloat("Lon##stereo", &projections_stereo_center_lon);
                ImGui::Spacing();
                ImGui::InputFloat("Масштаб (м/пикс)##stereo", &projections_stereo_scale);
            }
            else if (projections_current_selected_proj == 3)
            {
                ImGui::Text("Координаты центра:");
                ImGui::InputFloat("Lat##tpers", &projections_tpers_lat);
                ImGui::InputFloat("Lon##tpers", &projections_tpers_lon);
                ImGui::Spacing();
                ImGui::InputFloat("Высота (м)##tpers", &projections_tpers_alt);
                ImGui::InputFloat("Наклон##tpers", &projections_tpers_ang);
                ImGui::InputFloat("Азимут##tpers", &projections_tpers_azi);
                ImGui::Spacing();
                ImGui::InputFloat("Масштаб##tpers", &projections_tpers_scale);
            }
            /*else if (projections_current_selected_proj == 4)
            {
                ImGui::Text("Center Coordinates :");
                ImGui::InputFloat("Lat##eqaz", &projections_azeq_lat);
                ImGui::InputFloat("Lon##eqaz", &projections_azeq_lon);
            }*/

            if (projections_current_selected_proj == 0 || projections_current_selected_proj == 2)
            {
                ImGui::Checkbox("Автоматический режим###pojautomode", &projection_auto_mode);
                ImGui::Checkbox("Автоматический масштаб##projautoscalemode", &projection_auto_scale_mode);
                if (projection_auto_scale_mode)
                {
                    ImGui::InputDouble("Масштаб X (м/пикс)##projscalexauto", &projection_autoscale_x);
                    ImGui::InputDouble("Масштаб Y (м/пикс)##projscalexauto", &projection_autoscale_y);
                }
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (disable_buttons || active_layers == 0)
                style::beginDisabled();
            if (ImGui::Button("Сгенерировать проекцию"))
            {
                auto fun = [this](int)
                {
                    logger->info("Обновление проекции...");
                    try
                    {
                        generateProjectionImage();
                    }
                    catch (std::exception &e)
                    {
                        logger->error("Ошибка генерации проекции:\n%s", e.what());
                        projections_are_generating = false;
                    }
                    logger->info("Готово");
                };
                ui_thread_pool.push(fun);
            }
            if (projections_are_generating)
            {
                ImGui::SameLine();
                widgets::Spinner("###spinner1", ImGui::GetItemRectSize().y / 2 - ImGui::GetStyle().FramePadding.y, 3 * ui_scale, ImGui::GetColorU32(ImGuiCol_Text));
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                if (projections_are_generating)
                    ImGui::SetTooltip("Генерация, пожалуйста подождите...");
                if (projection_layers.size() == 0)
                    ImGui::SetTooltip("Слои не загружены");
                if (active_layers == 0)
                    ImGui::SetTooltip("Нет активных слоёв для проекции");
            }

            ImGui::Spacing();

            if (ImGui::Button("Сохранить проекцию"))
            {
                ui_thread_pool.push([this](int)
                                    {   projections_are_generating = true;
                        logger->info("Сохранение проекции...");
                        std::string default_path = config::main_cfg["satdump_directories"]["default_projection_output_directory"]["value"].get<std::string>();
                        std::string saved_at = save_image_dialog("projection", default_path, "Сохранить проекцию", &projected_image_result, &viewer_app->save_type);

                        if (saved_at == "")
                            logger->info("Сохранение отменено");
                        else
                            logger->info("Проекция сохранена: %s", saved_at.c_str());
                        projections_are_generating = false; });
            }

            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                if (projections_are_generating)
                    ImGui::SetTooltip("Генерация, пожалуйста подождите...");
                if (projection_layers.size() == 0)
                    ImGui::SetTooltip("Слои не загружены");
                if (active_layers == 0)
                    ImGui::SetTooltip("Нет активных слоёв для проекции");
            }
            if (disable_buttons || active_layers == 0)
                style::endDisabled();
        }
        if (ImGui::CollapsingHeader("Слои"))
        {
            ImGui::BeginGroup();
            ImGui::Text("Режим:");
            ImGui::SameLine();
            ImGui::Text("Смешивание");
            ImGui::SameLine();

            ImGui::SetNextItemWidth(50);
            ToggleButton("##projtog", &projections_mode_radio);
            ImGui::SameLine();
            ImGui::Text("Наложение");
            ImGui::EndGroup();

            ImGui::Separator(); //////////////////////////////////////////////////////

            ImGui::Text("Слои:");

            if (disable_buttons)
                style::beginDisabled();

            ImGui::SameLine();
            ImGuiStyle &imguistyle = ImGui::GetStyle();
            ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - ((projections_loading_new_layer ? 16.0 * ui_scale + imguistyle.ItemSpacing.x : 0) +
                                                                  ImGui::CalcTextSize("Добавить слой").x + ImGui::CalcTextSize("Все").x + ImGui::CalcTextSize("Ничего").x +
                                                                  imguistyle.FramePadding.x * 6 + imguistyle.ItemSpacing.x * 2));

            if (projections_loading_new_layer)
            {
                widgets::Spinner("###spinner1", 8 * ui_scale, 3 * ui_scale, ImGui::GetColorU32(ImGuiCol_Text));
                ImGui::SameLine();
            }

            if (ImGui::Button("Добавить слой##button"))
                ImGui::OpenPopup("Добавить слой##popup", ImGuiPopupFlags_None);

            if (disable_buttons)
                style::endDisabled();

            {
                if (ImGui::BeginPopupModal("Добавить слой##popup", NULL, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize))
                {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::RadioButton("Эквидистантная цилиндрическая", &selected_external_type, 0);
                    ImGui::RadioButton("Тайловая карта (OSM)", &selected_external_type, 1);
                    ImGui::RadioButton("GeoTIFF", &selected_external_type, 2);
                    ImGui::RadioButton("Другое", &selected_external_type, 3);

                    if (selected_external_type == 0 ||
                        selected_external_type == 2 ||
                        selected_external_type == 3)
                    {
                        ImGui::InputText("Название", &projection_new_layer_name);
                        projection_new_layer_file.draw("Входное изображение");
                        if (selected_external_type == 3)
                            projection_new_layer_cfg.draw("Файл конфигурации проекции");
                        ImGui::Checkbox("Нормализовать###normalizeinput", &projection_normalize_image);
                    }
                    else if (selected_external_type == 1)
                    {
                        ImGui::InputDouble("Lat1##osmlat1", &projection_osm_lat1);
                        ImGui::InputDouble("Lon1##osmlat1", &projection_osm_lon1);
                        ImGui::InputDouble("Lat2##osmlat1", &projection_osm_lat2);
                        ImGui::InputDouble("Lon2##osmlat1", &projection_osm_lon2);
                        ImGui::SliderInt("Масштаб##osmsliderzoom", &projection_osm_zoom, 0, 6);
                        if (!urlgood)
                            ImGui::PushStyleColor(ImGuiCol_Text, style::theme.red.Value);

                        ImGui::InputText("URL тайлов", &mapurl, ImGuiInputTextFlags_None);

                        if (!urlgood)
                        {
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("Некорректный URL. Должно быть: https://server/{z}/{x}/{y}.ext");
                            ImGui::PopStyleColor();
                        }
                    }

                    if (ImGui::Button("Добавить слой"))
                    {
                        auto func = [this](int)
                        {
                            LayerLoadingConfig cfg;
                            if (selected_external_type == 0)
                                cfg.type = "equirectangular";
                            else if (selected_external_type == 1)
                                cfg.type = "tilemap";
                            else if (selected_external_type == 2)
                                cfg.type = "geotiff";
                            else if (selected_external_type == 3)
                                cfg.type = "other";

                            cfg.file = projection_new_layer_file.getPath();
                            cfg.normalize = projection_normalize_image;
                            cfg.projfile = projection_new_layer_cfg.getPath();

                            if (selected_external_type == 1) // TODO: Move to reprojector backend
                            {
                                try
                                {
                                    logger->info("Генерация тайловой карты");
                                    image::Image timemap = downloadTileMap(mapurl, projection_osm_lat1, projection_osm_lon1, projection_osm_lat2, projection_osm_lon2, projection_osm_zoom);

                                    projection_layers_mtx.lock();
                                    projection_layers.push_front({"Tile Map", timemap});
                                    projection_layers_mtx.unlock();
                                }
                                catch (std::exception &e)
                                {
                                    logger->error("Не удалось загрузить тайловую карту: %s", e.what());
                                    projections_loading_new_layer = false;
                                }
                            }
                            else
                            {
                                try
                                {
                                    ProjectionLayer newlayer = satdump::loadExternalLayer(cfg);
                                    newlayer.name = projection_new_layer_name;
                                    projection_layers_mtx.lock();
                                    projection_layers.push_front(newlayer);
                                    projection_layers_mtx.unlock();
                                }
                                catch (std::exception &e)
                                {
                                    logger->error(e.what());
                                }
                            }

                            projections_loading_new_layer = false;
                        };

                        ui_thread_pool.push(func);
                        projections_loading_new_layer = true;

                        ImGui::CloseCurrentPopup();
                    }

                    ImGui::SameLine();

                    if (ImGui::Button("Отмена"))
                        ImGui::CloseCurrentPopup();

                    ImGui::EndPopup();
                }
            }

            ImGui::SameLine();
            projection_layers_mtx.lock();
            if (active_layers == static_cast<int>(projection_layers.size()))
                style::beginDisabled();
            if (ImGui::Button("Все"))
            {
                for (auto &lay : projection_layers)
                    lay.enabled = true;
                logger->info("Все слои проекции включены");
            }
            if (active_layers == static_cast<int>(projection_layers.size()))
                style::endDisabled();

            ImGui::SameLine();

            if (active_layers == 0)
                style::beginDisabled();
            if (ImGui::Button("Ничего"))
            {
                for (auto &lay : projection_layers)
                    lay.enabled = false;
                logger->info("Все слои проекции отключены");
            }
            if (active_layers == 0)
                style::endDisabled();
            projection_layers_mtx.unlock();

            if (ImGui::BeginListBox("##projectionslistbox", ImVec2(ImGui::GetWindowWidth(), 300 * ui_scale)))
            {
                projection_layers_mtx.lock();
                for (int i = 0; i < (int)projection_layers.size(); i++)
                {
                    ImGui::PushID(i);
                    ImGui::BeginGroup();
                    ProjectionLayer &layer = projection_layers[i];
                    std::string label;
                    label = layer.name;
                    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPos().x, ImGui::GetCursorPos().y + 4 * ui_scale));
                    ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - 70 * ui_scale);
                    ImGui::Text("%s", label.c_str());
                    ImGui::PopTextWrapPos();

                    if (settings::advanced_mode)
                    {
                        ImGui::SameLine(ImGui::GetWindowWidth() - 100 * ui_scale);
                        ImGui::SetCursorPosY(ImGui::GetCursorPos().y - 2 * ui_scale);
                        ImGui::Checkbox(std::string("##enablelayersettings" + layer.name + std::to_string(i)).c_str(), &layer.allow_editor);
                    }

                    ImGui::SameLine(ImGui::GetWindowWidth() - 70 * ui_scale);
                    ImGui::SetCursorPosY(ImGui::GetCursorPos().y - 2 * ui_scale);
                    ImGui::Checkbox(std::string("##enablelayer" + layer.name + std::to_string(i)).c_str(), &layer.enabled);

                    {
                        if (disable_buttons)
                            ImGui::BeginDisabled();
                        // Closing button
                        ImGui::SameLine();
                        ImGui::SetCursorPosY(ImGui::GetCursorPos().y - 2 * ui_scale);
                        ImGui::PushStyleColor(ImGuiCol_Text, style::theme.red.Value);
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4());
                        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
                        if (ImGui::Button(std::string(u8"\uf00d##layerdelete" + layer.name + std::to_string(i)).c_str()))
                        {
                            projection_layers.erase(projection_layers.begin() + i);
                            ImGui::PopStyleVar();
                            ImGui::PopStyleColor(2);
                            ImGui::EndGroup();
                            ImGui::PopID();
                            break;
                        }
                        if (projections_are_generating && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            ImGui::SetTooltip("Дождитесь завершения обработки");
                        ImGui::PopStyleVar();
                        ImGui::PopStyleColor(2);
                        if (disable_buttons)
                            ImGui::EndDisabled();
                    }

                    if (layer.enabled)
                    {
                        // ImGui::DragFloat(std::string("Opacity##opacitylayer" + layer.name + std::to_string(i)).c_str(), &layer.opacity, 1.0, 0, 100);
                        ImGui::Image((void *)(intptr_t)layer.getPreview(), {50 * ui_scale, 50 * ui_scale});
                        ImGui::SameLine();

                        ImGui::BeginGroup();
                        if (projections_mode_radio == 0)
                            style::beginDisabled();
                        FancySlider(std::string("##opacitylayer" + layer.name + std::to_string(i)).c_str(), "Непрозрачность", &layer.opacity, ImGui::GetWindowWidth() - 76 * ui_scale);
                        if (projections_mode_radio == 0)
                        {
                            ImGui::SetItemTooltip("%s", "Непрозрачность используется только в режиме наложения");
                            style::endDisabled();
                        }
                        ImGui::ProgressBar(layer.progress, ImVec2(ImGui::GetWindowWidth() - 76 * ui_scale, ImGui::GetFrameHeight()));

                        if (layer.allow_editor)
                        {
                            ImGui::Separator();
                            auto js = image::get_metadata_proj_cfg(layer.img);
                            widgets::JSONTableEditor(js, "proj_cfg");
                            image::set_metadata_proj_cfg(layer.img, js);
                            ImGui::Separator();
                        }

                        ImGui::EndGroup();
                    }
                    ImGui::EndGroup();

                    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
                    {
                        // Set payload to carry the index of our item (could be anything)
                        ImGui::SetDragDropPayload("LAYER_PROJECTION", &i, sizeof(int));

                        ImGui::Text("%s", label.c_str());
                        ImGui::EndDragDropSource();
                    }
                    if (ImGui::BeginDragDropTarget())
                    {
                        if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("LAYER_PROJECTION"))
                        {
                            IM_ASSERT(payload->DataSize == sizeof(int));
                            int payload_n = *(const int *)payload->Data;
                            ProjectionLayer pr = projection_layers[payload_n];
                            projection_layers.erase(projection_layers.begin() + payload_n);
                            auto it = projection_layers.begin();
                            projection_layers.insert(it + i, pr);
                        }
                        ImGui::EndDragDropTarget();
                    }
                    ImGui::PopID();
                    ImGui::Separator();
                }
                projection_layers_mtx.unlock();
                ImGui::EndListBox();
            }
            if (!disable_buttons)
                style::beginDisabled();

            ImGui::ProgressBar((general_progress + (progress_pointer == nullptr ? 0 : *(progress_pointer))) / general_sum);

            if (!disable_buttons)
                style::endDisabled();
        }
        if (ImGui::CollapsingHeader("Оверлей##viewerpojoverlay"))
        {
            projection_overlay_handler.drawUI();
        }
    }

    void ViewerApplication::generateProjectionImage()
    {
        projections_are_generating = true;
        general_progress = 0;
        general_sum = 0;

        nlohmann::json cfg;

        if (projections_current_selected_proj == 0)
        {
            cfg["type"] = "equirec";
            cfg["offset_x"] = projections_equirectangular_tl_lon;
            cfg["offset_y"] = projections_equirectangular_tl_lat;
            cfg["scalar_x"] = (projections_equirectangular_br_lon - projections_equirectangular_tl_lon) / double(projections_image_width);
            cfg["scalar_y"] = (projections_equirectangular_br_lat - projections_equirectangular_tl_lat) / double(projections_image_height);
        }
        else if (projections_current_selected_proj == 1)
        {
            cfg["type"] = "utm";
            cfg["scale"] = projections_utm_scale;
            cfg["zone"] = projections_utm_zone;
            cfg["south"] = projections_utm_south;
            cfg["offset_y"] = projections_utm_offset_y;
        }
        else if (projections_current_selected_proj == 2)
        {
            cfg["type"] = "stereo";
            cfg["center_lon"] = projections_stereo_center_lon;
            cfg["center_lat"] = projections_stereo_center_lat;
            cfg["scale"] = projections_stereo_scale;
            cfg["width"] = projections_image_width;
            cfg["height"] = projections_image_height;
        }
        else if (projections_current_selected_proj == 3)
        {
            cfg["type"] = "tpers";
            cfg["center_lon"] = projections_tpers_lon;
            cfg["center_lat"] = projections_tpers_lat;
            cfg["altitude"] = projections_tpers_alt;
            cfg["tilt"] = projections_tpers_ang;
            cfg["azimuth"] = projections_tpers_azi;
            cfg["scale"] = projections_tpers_scale;
            cfg["width"] = projections_image_width;
            cfg["height"] = projections_image_height;
        }
        /*else if (projections_current_selected_proj == 4)
        {
            cfg["type"] = "azeq";
            cfg["lon"] = projections_azeq_lon;
            cfg["lat"] = projections_azeq_lat;
        }*/

        // Automatic projection settings!
        if (projection_auto_scale_mode)
        {
            cfg["scale_x"] = projection_autoscale_x;
            cfg["scale_y"] = projection_autoscale_y;
        }
        else
        {
            cfg["width"] = projections_image_width;
            cfg["height"] = projections_image_height;
        }

        satdump::applyAutomaticProjectionSettings(projection_layers, projection_auto_mode, projection_auto_scale_mode, projections_image_width, projections_image_height, cfg);

        for (int i = projection_layers.size() - 1; i >= 0; i--)
        {
            ProjectionLayer &layer = projection_layers[i];
            if (!layer.enabled)
                continue;
            general_sum++;
        }
        general_sum += projection_overlay_handler.enabled();

        // Generate all layers
        std::vector<image::Image> layers_images =
            generateAllProjectionLayers(projection_layers, projections_image_width, projections_image_height, cfg, &general_progress);

        logger->info("Объединение изображений...");
        if (projections_mode_radio == 0) // Blend
        {
            projected_image_result = image::blend_images(layers_images);
        }
        else if (projections_mode_radio == 1) // Overlay
        {
            projected_image_result = image::Image(16, layers_images[0].width(), layers_images[0].height(), layers_images[0].channels());
            for (int i = 0; i < (int)layers_images.size(); i++)
            {
                projected_image_result = image::merge_images_opacity(projected_image_result,
                                                                     layers_images[i],
                                                                     projection_layers[(projection_layers.size() - 1) - i].opacity / 100.0f);
            }
        }

        // Copy projection metadata
        if (image::has_metadata(layers_images[0]))
            image::set_metadata(projected_image_result, image::get_metadata(layers_images[0]));

        // Free up memory
        layers_images.clear();

        logger->info("Наложение оверлеев...");

        // Setup projection to draw stuff on top
        auto proj_func = satdump::reprojection::setupProjectionFunction(projections_image_width, projections_image_height, cfg, {});

        // Draw map borders
        projection_overlay_handler.clear_cache();
        projection_overlay_handler.apply(projected_image_result, proj_func, &general_progress);

        // Update ImageView
        projection_image_widget.update(projected_image_result);
        general_sum = 1;
        general_progress = 1;
        progress_pointer = nullptr;
        projections_are_generating = false;
    }
}
