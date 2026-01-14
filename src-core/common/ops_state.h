#pragma once

#include <string>

namespace satdump::ops
{
    struct FirstValidFrameEvent
    {
        std::string run_id;
        std::string source;
    };

    struct RunFinalizedEvent
    {
        std::string run_id;
        std::string output_dir;
    };

    struct FifoDeleteEvent
    {
        std::string run_id;
        std::string output_dir;
    };

    struct OpsStateSnapshot
    {
        bool pipeline_active = false;
        bool first_valid_frame = false;
        bool run_finalized = false;
        bool fifo_delete = false;
        std::string live_run_id;
        std::string live_tmp_dir;
        std::string live_final_dir;
        double live_start_timestamp = 0.0;
        std::string last_finalized_run_id;
        std::string last_deleted_run_id;
        std::string last_event;
    };

    void register_event_handlers();
    OpsStateSnapshot get_state();
    void set_live_run(const std::string &run_id,
                      const std::string &tmp_dir,
                      const std::string &final_dir,
                      double start_timestamp);
    void set_pipeline_active(bool active);
    std::string normalize_run_id(const std::string &name);
    bool is_temp_run_dir(const std::string &name);
    std::string build_temp_run_dir(const std::string &final_dir);
}
