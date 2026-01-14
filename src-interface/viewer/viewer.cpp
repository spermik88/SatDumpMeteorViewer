#include "viewer.h"
#include "image_handler.h"
#include "radiation_handler.h"
#include "scatterometer_handler.h"
#include "core/config.h"
#include "products/dataset.h"
#include "common/utils.h"
#include "resources.h"
#include "main_ui.h"
#include "common/image/image_utils.h"
#include <algorithm>
#include <cmath>

void SelectableColor(ImU32 color) // Colors a cell in the table with the specified color in RGBA
{
    ImVec2 p_min = ImGui::GetItemRectMin();
    ImVec2 p_max = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRectFilled(p_min, p_max, color);
}

namespace satdump
{
    ViewerApplication::ViewerApplication()
        : Application("viewer")
    {
        projection_overlay_handler.draw_map_overlay = true;
        projection_overlay_handler.draw_cities_overlay = true;

        if (config::main_cfg["user"].contains("viewer_state"))
        {
            if (config::main_cfg["user"]["viewer_state"].contains("panel_ratio"))
                panel_ratio = config::main_cfg["user"]["viewer_state"]["panel_ratio"].get<float>();

            if (config::main_cfg["user"]["viewer_state"].contains("save_type"))
                save_type = config::main_cfg["user"]["viewer_state"]["save_type"].get<std::string>();
            else
                save_type = satdump::config::main_cfg["satdump_general"]["image_format"]["value"].get<std::string>();

            if (config::main_cfg["user"]["viewer_state"].contains("projections"))
                deserialize_projections_config(config::main_cfg["user"]["viewer_state"]["projections"]);
        }

        std::string default_dir = config::main_cfg["satdump_directories"]["default_input_directory"]["value"].get<std::string>();
        projection_new_layer_file.setDefaultDir(default_dir);
        projection_new_layer_cfg.setDefaultDir(default_dir);
        select_dataset_products_dialog.setDefaultDir(default_dir);

        // pro.load("/home/alan/Documents/SatDump_ReWork/build/metop_ahrpt_new/AVHRR/product.cbor");
        //  pro.load("/home/alan/Documents/SatDump_ReWork/build/aqua_test_new/MODIS/product.cbor");

        // loadDatasetInViewer("/home/alan/Documents/SatDump_ReWork/build/metop_ahrpt_new/dataset.json");
        // loadDatasetInViewer("/home/alan/Documents/SatDump_ReWork/build/metop_idk_damnit/dataset.json");

        // loadDatasetInViewer("/tmp/hirs/dataset.json");

        // loadDatasetInViewer("/home/zbyszek/Downloads/metopC_15-04_1125/dataset.json");

        // loadProductsInViewer("/home/alan/Documents/SatDump_ReWork/build/noaa_mhs_test/AMSU/product.cbor", "NOAA-19 HRPT");
        // loadProductsInViewer("/home/alan/Documents/SatDump_ReWork/build/metop_ahrpt_new/AVHRR/product.cbor", "MetOp-B AHRPT");
        // loadProductsInViewer("/home/alan/Documents/SatDump_ReWork/build/noaa_mhs_test/MHS/product.cbor", "NOAA-19 HRPT");
        // loadProductsInViewer("/home/alan/Documents/SatDump_ReWork/build/noaa_mhs_test_gac/MHS/product.cbor", "NOAA-19 GAC");
        // loadProductsInViewer("/home/alan/Documents/SatDump_ReWork/build/noaa_mhs_test_gac/HIRS/product.cbor", "NOAA-19 GAC");
        // loadProductsInViewer("/home/alan/Documents/SatDump_ReWork/build/noaa_mhs_test_gac/AVHRR/product.cbor", "NOAA-19 GAC");
        // loadProductsInViewer("/home/alan/Documents/SatDump_ReWork/build/npp_hrd_new/VIIRS/product.cbor", "JPSS-1 HRD");
        // loadProductsInViewer("/home/alan/Documents/SatDump_ReWork/build/m2_lrpt_test/MSU-MR/product.cbor", "METEOR-M2 LRPT");
    }

    ViewerApplication::~ViewerApplication()
    {
    }

    void ViewerApplication::save_settings()
    {
        config::main_cfg["user"]["viewer_state"]["panel_ratio"] = panel_ratio;
        config::main_cfg["user"]["viewer_state"]["save_type"] = save_type;
        config::main_cfg["user"]["viewer_state"]["projections"] = serialize_projections_config();
    }

    void ViewerApplication::loadDatasetInViewer(std::string path)
    {
        ProductDataSet dataset;
        dataset.load(path);

        std::string dataset_name = dataset.satellite_name + " " + timestamp_to_string(dataset.timestamp);

        int i = -1;
        bool contains = false;
        do
        {
            contains = false;
            std::string curr_name = ((i + 1) == 0 ? dataset_name : (dataset_name + " #" + std::to_string(i + 1)));
            for (int i = 0; i < (int)products_and_handlers.size(); i++)
                if (products_and_handlers[i]->dataset_name == curr_name)
                    contains = true;
            i++;
        } while (contains);
        dataset_name = (i == 0 ? dataset_name : (dataset_name + " #" + std::to_string(i)));

        std::string pro_directory = std::filesystem::path(path).parent_path().string();
        for (std::string pro_path : dataset.products_list)
        {
            try
            {
                if (path.find("http") == 0)
                {
                    // Make sure the path is URL safe
                    std::ostringstream encodedUrl;
                    encodedUrl << std::hex << std::uppercase << std::setfill('0');
                    for (char &c : pro_path)
                    {
                        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
                            encodedUrl << c;
                        else
                            encodedUrl << '%' << std::setw(2) << static_cast<unsigned int>(static_cast<unsigned char>(c));
                    }
                    pro_path = encodedUrl.str();
                }
                loadProductsInViewer(pro_directory + "/" + pro_path, dataset_name);
            }
            catch (std::exception &e)
            {
                logger->error("Could not open " + pro_path + " in viewer! : %s", e.what());
            }
        }
    }

    void ViewerApplication::loadProductsInViewer(std::string path, std::string dataset_name)
    {
        if (std::filesystem::exists(path) || path.find("http") == 0)
        {
            std::shared_ptr<Products> products = loadProducts(path);

            // Get instrument settings
            nlohmann::ordered_json instrument_viewer_settings;
            if (config::main_cfg["viewer"]["instruments"].contains(products->instrument_name))
                instrument_viewer_settings = config::main_cfg["viewer"]["instruments"][products->instrument_name];
            else
                logger->error("Unknown instrument : %s!", products->instrument_name.c_str());

            // Init Handler
            std::string handler_id;
            if (instrument_viewer_settings.contains("handler"))
                handler_id = instrument_viewer_settings["handler"].get<std::string>();
            else if (products->contents["type"] == "image")
                handler_id = "image_handler";
            else if (products->contents["type"] == "radiation")
                handler_id = "radiation_handler";
            else if (products->contents["type"] == "scatterometer")
                handler_id = "scatterometer_handler";
            logger->debug("Using handler %s for instrument %s", handler_id.c_str(), products->instrument_name.c_str());
            std::shared_ptr<ViewerHandler> handler = viewer_handlers_registry[handler_id]();

            handler->products = products.get();
            handler->instrument_cfg = instrument_viewer_settings;
            handler->init();

            if (dataset_name != "")
            {
                bool dataset_exists = false;
                for (std::string dataset_name2 : opened_datasets)
                    dataset_exists = dataset_exists | (dataset_name2 == dataset_name);

                if (!dataset_exists)
                    opened_datasets.push_back(dataset_name);
            }

            // Push products and handler
            product_handler_mutex.lock();
            products_and_handlers.push_back(std::make_shared<ProductsHandler>(products, handler, dataset_name));
            product_handler_mutex.unlock();
        }
    }

    ImRect ViewerApplication::renderHandler(ProductsHandler &ph, int index)
    {
        std::string label = ph.products->instrument_name;
        if (ph.handler->instrument_cfg.contains("name"))
            label = ph.handler->instrument_cfg["name"].get<std::string>();
        if (ph.products->has_product_source())
            label = ph.products->get_product_source() + " " + label;
        if (ph.products->has_product_timestamp())
            label = label + " " + timestamp_to_string(ph.products->get_product_timestamp());

        ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | (index == current_handler_id ? ImGuiTreeNodeFlags_Selected : 0));
        if (ImGui::IsItemClicked())
            current_handler_id = index;

        if (index == current_handler_id && ph.dataset_name == "")
        { // Closing button
            ImGui::SameLine();
            ImGui::Text("  ");
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Text, style::theme.red.Value);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4());
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
            if (ImGui::SmallButton(std::string(u8"\uf00d##" + ph.dataset_name + label).c_str()))
            {
                logger->info("Closing products " + label);
                ph.marked_for_close = true;
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
        }

        ImRect rect = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

        if (index == current_handler_id)
        {
            ImGui::TreePush(std::string("##HandlerTree" + std::to_string(current_handler_id)).c_str());
            products_and_handlers[current_handler_id]->handler->drawTreeMenu();
            ImGui::TreePop();
        }
        // ImGui::InputInt("Current Product", &current_handler_id);

        return rect;
    }

    bool ViewerApplication::isLayerAvailable(size_t index) const
    {
        if (index >= kLayerCount)
            return false;
        return layer_set.available[index];
    }

    bool ViewerApplication::isLayerEnabled(size_t index) const
    {
        if (index >= kLayerCount)
            return false;
        return layer_enabled[index];
    }

    void ViewerApplication::setLayerEnabled(size_t index, bool enabled)
    {
        if (index >= kLayerCount || !layer_set.available[index])
            return;

        if (layer_mode == LayerMode::Single)
        {
            if (!enabled)
                return;
            layer_enabled.fill(false);
            layer_enabled[index] = true;
        }
        else
        {
            layer_enabled[index] = enabled;
        }

        markLayerCompositeDirty();
    }

    bool ViewerApplication::isPreviewAvailable() const
    {
        return layer_set.preview_available;
    }

    bool ViewerApplication::isPreviewEnabled() const
    {
        return preview_enabled && layer_set.preview_available;
    }

    void ViewerApplication::setPreviewEnabled(bool enabled)
    {
        if (!layer_set.preview_available)
            return;
        if (preview_enabled == enabled)
            return;
        preview_enabled = enabled;
        markLayerCompositeDirty();
    }

    size_t ViewerApplication::getEnabledStackLayerCount() const
    {
        size_t count = 0;
        for (size_t i = 0; i < kLayerCount; ++i)
            if (layer_enabled[i] && layer_set.available[i])
                ++count;
        return count;
    }

    ViewerApplication::LayerMode ViewerApplication::getLayerMode() const
    {
        return layer_mode;
    }

    void ViewerApplication::setLayerMode(LayerMode mode)
    {
        if (layer_mode == mode)
            return;
        layer_mode = mode;
        updateLayerSelectionsForMode();
        markLayerCompositeDirty();
    }

    void ViewerApplication::updateLayerModelFromHandler(const std::shared_ptr<ViewerHandler> &handler)
    {
        LayerSet new_layer_set{};
        const Products *new_source = nullptr;
        uint64_t new_preview_revision = 0;
        std::string run_id = selected_run_id;
        bool run_changed = run_id != layer_run_id;
        if (run_changed)
        {
            layer_run_id = run_id;
            layer_run_epoch++;
            if (layer_run_epoch == 0)
                layer_run_epoch = 1;
        }

        int default_layer_index = -1;
        if (handler)
        {
            if (auto image_handler_local = dynamic_cast<ImageViewerHandler *>(handler.get()))
            {
                new_source = image_handler_local->products;
                default_layer_index = image_handler_local->active_channel_id;
                if (image_handler_local->current_image.width() > 0)
                {
                    new_layer_set.preview = &image_handler_local->current_image;
                    new_layer_set.preview_available = true;
                    new_preview_revision = image_handler_local->current_image_revision;
                }

                if (image_handler_local->products)
                {
                    size_t layer_count = std::min(image_handler_local->products->images.size(), kLayerCount);
                    for (size_t i = 0; i < layer_count; ++i)
                    {
                        new_layer_set.layers[i] = &image_handler_local->products->images[i].image;
                        if (new_layer_set.layers[i])
                            new_layer_set.available[i] = new_layer_set.layers[i]->width() > 0;
                    }
                }
            }
        }

        bool source_changed = new_source != layer_products_source;
        bool availability_changed = new_layer_set.available != layer_set.available ||
            new_layer_set.preview_available != layer_set.preview_available;
        bool preview_ptr_changed = new_layer_set.preview != layer_set.preview;
        bool layers_ptr_changed = new_layer_set.layers != layer_set.layers;

        for (size_t i = 0; i < kLayerCount; ++i)
        {
            if (!new_layer_set.available[i] || !new_layer_set.layers[i])
            {
                layer_revisions[i] = 0;
                continue;
            }
            if (run_changed || new_layer_set.layers[i] != layer_set.layers[i] || layer_revisions[i] == 0)
                layer_revisions[i] = layer_run_epoch;
        }

        if (new_layer_set.preview_available && new_layer_set.preview)
        {
            uint64_t preview_revision_value = std::max<uint64_t>(new_preview_revision, 1);
            preview_revision = (layer_run_epoch << 32) | (preview_revision_value & 0xFFFFFFFFull);
        }
        else
        {
            preview_revision = 0;
        }

        layer_set = new_layer_set;
        layer_products_source = new_source;

        if (source_changed || availability_changed || preview_ptr_changed || layers_ptr_changed || run_changed)
        {
            if (!layer_set.preview_available)
                preview_enabled = false;
            if (source_changed && layer_mode == LayerMode::Single)
            {
                layer_enabled.fill(false);
                if (default_layer_index >= 0 &&
                    default_layer_index < static_cast<int>(kLayerCount) &&
                    layer_set.available[static_cast<size_t>(default_layer_index)])
                    layer_enabled[static_cast<size_t>(default_layer_index)] = true;
            }
            updateLayerSelectionsForMode();
            markLayerCompositeDirty();
        }

        if (!layer_set.preview_available && preview_enabled)
            preview_enabled = false;
    }

    int ViewerApplication::resolveSingleLayerSelection() const
    {
        for (size_t i = 0; i < kLayerCount; ++i)
            if (layer_enabled[i] && layer_set.available[i])
                return static_cast<int>(i);

        for (size_t i = 0; i < kLayerCount; ++i)
            if (layer_set.available[i])
                return static_cast<int>(i);

        return -1;
    }

    void ViewerApplication::updateLayerSelectionsForMode()
    {
        if (layer_mode == LayerMode::Single)
        {
            int selected_index = resolveSingleLayerSelection();
            layer_enabled.fill(false);
            if (selected_index >= 0)
                layer_enabled[static_cast<size_t>(selected_index)] = true;
        }
        else
        {
            for (size_t i = 0; i < kLayerCount; ++i)
                if (!layer_set.available[i])
                    layer_enabled[i] = false;
        }
    }

    void ViewerApplication::updateLayerComposite()
    {
        bool selection_changed = last_layer_enabled != layer_enabled ||
            last_layer_mode != layer_mode ||
            last_preview_enabled != preview_enabled ||
            last_layer_ptrs != layer_set.layers ||
            last_layer_revisions != layer_revisions ||
            last_preview_ptr != layer_set.preview ||
            last_preview_revision != preview_revision;

        if (!layer_composite_dirty && !selection_changed)
            return;

        layer_composite_dirty = false;
        last_layer_enabled = layer_enabled;
        last_layer_ptrs = layer_set.layers;
        last_layer_revisions = layer_revisions;
        last_preview_ptr = layer_set.preview;
        last_preview_revision = preview_revision;
        last_layer_mode = layer_mode;
        last_preview_enabled = preview_enabled;

        if (layer_mode == LayerMode::Single)
        {
            stack_layers_warning = false;
            int selected_index = resolveSingleLayerSelection();
            if (selected_index >= 0 && layer_set.layers[static_cast<size_t>(selected_index)])
            {
                layer_view.plotOverlay = []() {};
                layer_view.updateCached(layer_set.layers[static_cast<size_t>(selected_index)],
                    layer_revisions[static_cast<size_t>(selected_index)],
                    1.0f);
            }
            else
            {
                layer_view.plotOverlay = []() {};
                layer_view.updateCached(nullptr, 0, 1.0f);
            }
        }
        else
        {
            size_t enabled_count = getEnabledStackLayerCount();
            stack_layers_warning = enabled_count > 3;
            float overlay_alpha = stack_layers_warning ? 0.35f : 0.5f;

            const image::Image *base_image = nullptr;
            uint64_t base_revision = 0;
            size_t base_layer_index = kLayerCount;
            if (preview_enabled && layer_set.preview_available && layer_set.preview)
            {
                base_image = layer_set.preview;
                base_revision = preview_revision;
            }
            else
            {
                for (size_t i = 0; i < kLayerCount; ++i)
                {
                    if (layer_enabled[i] && layer_set.available[i] && layer_set.layers[i])
                    {
                        base_image = layer_set.layers[i];
                        base_revision = layer_revisions[i];
                        base_layer_index = i;
                        break;
                    }
                }
            }

            for (size_t i = 0; i < kLayerCount; ++i)
            {
                if (layer_enabled[i] && layer_set.available[i] && layer_set.layers[i])
                    stack_layer_views[i].updateCached(layer_set.layers[i], layer_revisions[i], overlay_alpha);
                else
                    stack_layer_views[i].updateCached(nullptr, 0, overlay_alpha);
            }

            layer_view.plotOverlay = [this, base_layer_index]() {
                for (size_t i = 0; i < kLayerCount; ++i)
                {
                    if (i == base_layer_index)
                        continue;
                    if (layer_enabled[i] && layer_set.available[i] && layer_set.layers[i])
                        stack_layer_views[i].plotChunks();
                }
            };

            if (base_image)
                layer_view.updateCached(base_image, base_revision, 1.0f);
            else
                layer_view.updateCached(nullptr, 0, 1.0f);
        }
    }

    void ViewerApplication::handleSwipePassNavigation(const ImRect &content_rect)
    {
        const float swipe_threshold = 80.0f * ui_scale;
        ImVec2 mouse_pos = ImGui::GetMousePos();
        bool hover_content = ImGui::IsMouseHoveringRect(content_rect.Min, content_rect.Max);

        if (!swipe_tracking && hover_content && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            swipe_tracking = true;
            swipe_start_pos = mouse_pos;
        }

        if (swipe_tracking && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            ImVec2 delta = ImVec2(mouse_pos.x - swipe_start_pos.x, mouse_pos.y - swipe_start_pos.y);
            if (std::abs(delta.x) > swipe_threshold && std::abs(delta.x) > std::abs(delta.y))
            {
                if (delta.x < 0)
                    switchPass(1);
                else
                    switchPass(-1);
            }
            swipe_tracking = false;
        }

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            swipe_tracking = false;
    }

    void ViewerApplication::switchPass(int offset)
    {
        if (opened_datasets.empty() || products_and_handlers.empty())
            return;

        const std::string &current_dataset = products_and_handlers[current_handler_id]->dataset_name;
        if (current_dataset.empty())
            return;

        auto it = std::find(opened_datasets.begin(), opened_datasets.end(), current_dataset);
        if (it == opened_datasets.end())
            return;

        int current_index = static_cast<int>(std::distance(opened_datasets.begin(), it));
        int target_index = current_index + offset;
        if (target_index < 0)
            target_index = static_cast<int>(opened_datasets.size()) - 1;
        else if (target_index >= static_cast<int>(opened_datasets.size()))
            target_index = 0;

        const std::string &target_dataset = opened_datasets[target_index];
        for (int i = 0; i < (int)products_and_handlers.size(); ++i)
        {
            if (products_and_handlers[i]->dataset_name == target_dataset)
            {
                current_handler_id = i;
                markLayerCompositeDirty();
                break;
            }
        }
    }

    void ViewerApplication::drawPanel()
    {
        if (ImGui::BeginTabBar("Viewer Prob Tabbar", ImGuiTabBarFlags_None))
        {
            ImGui::SetNextItemWidth(ImGui::GetWindowWidth() / 2);
            if (ImGui::BeginTabItem("Products###productsviewertab"))
            {
                if (current_selected_tab != 0)
                    current_selected_tab = 0;

                if (ImGui::CollapsingHeader("General", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    for (std::string dataset_name : opened_datasets)
                    {
                        if (products_cnt_in_dataset(dataset_name))
                        {
                            ImGui::TreeNodeEx(dataset_name.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
                            ImGui::TreePush(std::string("##HandlerTree" + dataset_name).c_str());

                            { // Closing button
                                ImGui::SameLine();
                                ImGui::Text("  ");
                                ImGui::SameLine();

                                ImGui::PushStyleColor(ImGuiCol_Text, style::theme.red.Value);
                                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4());
                                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
                                if (ImGui::SmallButton(std::string(u8"\uf00d##dataset" + dataset_name).c_str()))
                                {
                                    logger->info("Closing datset " + dataset_name);
                                    for (int i = 0; i < (int)products_and_handlers.size(); i++)
                                        if (products_and_handlers[i]->dataset_name == dataset_name)
                                        {
                                            products_and_handlers[i]->marked_for_close = true;
                                        }
                                }
                                ImGui::PopStyleVar();
                                ImGui::PopStyleColor(2);
                            }

                            const ImColor TreeLineColor = ImColor(128, 128, 128, 255); // ImGui::GetColorU32(ImGuiCol_Text);
                            const float SmallOffsetX = 11.0f;                          // for now, a hardcoded value; should take into account tree indent size
                            ImDrawList *drawList = ImGui::GetWindowDrawList();

                            ImVec2 verticalLineStart = ImGui::GetCursorScreenPos();
                            verticalLineStart.x += SmallOffsetX; // to nicely line up with the arrow symbol
                            ImVec2 verticalLineEnd = verticalLineStart;

                            for (int i = 0; i < (int)products_and_handlers.size(); i++)
                            {
                                if (products_and_handlers[i]->dataset_name == dataset_name)
                                {
                                    const float HorizontalTreeLineSize = 8.0f * ui_scale;                 // chosen arbitrarily
                                    const ImRect childRect = renderHandler(*products_and_handlers[i], i); // RenderTree(child);
                                    const float midpoint = (childRect.Min.y + childRect.Max.y) / 2.0f;
                                    drawList->AddLine(ImVec2(verticalLineStart.x, midpoint), ImVec2(verticalLineStart.x + HorizontalTreeLineSize, midpoint), TreeLineColor);
                                    verticalLineEnd.y = midpoint;
                                }
                            }

                            drawList->AddLine(verticalLineStart, verticalLineEnd, TreeLineColor);

                            ImGui::TreePop();
                        }
                    }

                    // Render unclassified
                    if (products_cnt_in_dataset(""))
                    {
                        ImGui::TreeNodeEx("Others", ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
                        ImGui::TreePush("##HandlerTreeOthers");
                        for (int i = 0; i < (int)products_and_handlers.size(); i++)
                            if (products_and_handlers[i]->dataset_name == "")
                                renderHandler(*products_and_handlers[i], i);
                        ImGui::TreePop();
                    }

                    // Handle deletion if required
                    for (int i = 0; i < (int)products_and_handlers.size(); i++)
                    {
                        if (products_and_handlers[i]->marked_for_close)
                        {
                            product_handler_mutex.lock();
                            products_and_handlers.erase(products_and_handlers.begin() + i);
                            product_handler_mutex.unlock();
                            if (current_handler_id >= (int)products_and_handlers.size())
                                current_handler_id = 0;
                            break;
                        }
                    }

                    ImGui::Separator();
                    ImGui::Text("Load Dataset/Products :");
                    if (select_dataset_products_dialog.draw())
                    {
                        ui_thread_pool.push([this](int)
                                            {
                            try
                            {
                                std::string path = select_dataset_products_dialog.getPath();
                                if(std::filesystem::path(path).extension() == ".json")
                                    loadDatasetInViewer(path);
                                else if(std::filesystem::path(path).extension() == ".cbor")
                                    loadProductsInViewer(path);
                                else
                                    logger->error("Invalid file! Not products or dataset!");
                            }
                            catch (std::exception &e)
                            {
                                logger->error("Error opening dataset/products - %s", e.what());
                            } });
                    }

                    eventBus->fire_event<RenderLoadMenuElementsEvent>({});
                }

                if (products_and_handlers.size() > 0)
                    products_and_handlers[current_handler_id]->handler->drawMenu();

                ImGui::EndTabItem();
            }

            ImGui::SetNextItemWidth(ImGui::GetWindowWidth() / 2);
            if (ImGui::BeginTabItem("Projections###projssviewertab"))
            {
                if (current_selected_tab != 1)
                    current_selected_tab = 1;
                drawProjectionPanel();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }

    void ViewerApplication::drawContent()
    {
    }

    void ViewerApplication::drawUI()
    {
        ImVec2 viewer_size = ImGui::GetContentRegionAvail();

        if (ImGui::BeginTable("##wiever_table", 2, ImGuiTableFlags_NoBordersInBodyUntilResize | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("##panel_v", ImGuiTableColumnFlags_None, viewer_size.x * panel_ratio);
            ImGui::TableSetupColumn("##view", ImGuiTableColumnFlags_None, viewer_size.x * (1.0f - panel_ratio));
            ImGui::TableNextColumn();

            float left_width = ImGui::GetColumnWidth(0);
            float right_width = viewer_size.x - left_width;
            if (left_width != last_width && last_width != -1)
                panel_ratio = left_width / viewer_size.x;
            last_width = left_width;

            ImGui::BeginChild("ViewerChildPanel", {left_width, float(viewer_size.y - 10)});
            drawPanel();
            ImGui::EndChild();

            ImGui::TableNextColumn();
            ImGui::BeginGroup();
            if (current_selected_tab == 0)
            {
                if (products_and_handlers.size() > 0)
                {
                    ImVec2 content_pos = ImGui::GetCursorScreenPos();
                    ImVec2 content_size = {float(right_width - 4), float(viewer_size.y)};
                    auto handler = products_and_handlers[current_handler_id]->handler;
                    updateLayerModelFromHandler(handler);
                    if (dynamic_cast<ImageViewerHandler *>(handler.get()))
                    {
                        updateLayerComposite();
                        if (layer_mode == LayerMode::Stack)
                        {
                            for (size_t i = 0; i < kLayerCount; ++i)
                                stack_layer_views[i].syncTextures();
                        }
                        layer_view.draw(content_size);
                    }
                    else
                    {
                        handler->drawContents(content_size);
                    }
                    handleSwipePassNavigation(ImRect(content_pos, ImVec2(content_pos.x + content_size.x, content_pos.y + content_size.y)));
                }
            }
            else if (current_selected_tab == 1)
            {
                projection_image_widget.draw({float(right_width - 4), float(viewer_size.y)});
            }
            ImGui::EndGroup();
            ImGui::EndTable();
        }
    }

    std::map<std::string, std::function<std::shared_ptr<ViewerHandler>()>> viewer_handlers_registry;
    void registerViewerHandlers()
    {
        viewer_handlers_registry.emplace(ImageViewerHandler::getID(), ImageViewerHandler::getInstance);
        viewer_handlers_registry.emplace(RadiationViewerHandler::getID(), RadiationViewerHandler::getInstance);
        viewer_handlers_registry.emplace(ScatterometerViewerHandler::getID(), ScatterometerViewerHandler::getInstance);
    }
};
