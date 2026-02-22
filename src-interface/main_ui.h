#pragma once

#include "dll_export.h"
#include <string>
#include <vector>
#include "libs/ctpl/ctpl_stl.h"
#include "app.h"
#include "viewer/viewer.h"
#include "recorder/recorder.h"
#include "common/tile_map/map.h"

namespace satdump
{
    enum class Screen
    {
        Viewer,
        Archive
    };

    SATDUMP_DLL2 extern bool update_ui;
    SATDUMP_DLL2 extern ctpl::thread_pool ui_thread_pool;

    SATDUMP_DLL2 extern std::shared_ptr<RecorderApplication> recorder_app;
    SATDUMP_DLL2 extern std::shared_ptr<ViewerApplication> viewer_app;
    SATDUMP_DLL2 extern Screen current_screen;
    SATDUMP_DLL2 extern std::string selected_run_id;

    bool is_appliance_mode();
    void invalidate_archive_index();
    std::vector<std::string> get_archive_run_ids();
    bool open_run_in_viewer(const std::string &run_id);

    void initMainUI();
    void exitMainUI();
    void renderMainUI();
}
