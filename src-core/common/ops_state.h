#pragma once

#include <string>

namespace satdump::ops
{
    enum class RxStage
    {
        WaitingSignal,
        Receiving,
        Decoding,
        Idle,
        Error
    };

    enum class SdrStage
    {
        Online,
        Offline,
        Error
    };

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

    struct ArchiveChangedEvent
    {
        std::string reason;
        std::string run_id;
    };

    struct OpsStateSnapshot
    {
        bool pipeline_active = false;
        bool first_valid_frame = false;
        bool run_finalized = false;
        bool fifo_delete = false;
        RxStage rx_stage = RxStage::Idle;
        SdrStage sdr_stage = SdrStage::Offline;
        std::string current_run_id;
        std::string current_run_tmp_dir;
        std::string current_run_final_dir;
        double last_first_valid_frame_ts = 0.0;
        double last_run_finalized_ts = 0.0;
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
    void set_rx_stage(RxStage stage);
    void set_sdr_stage(SdrStage stage);
    std::string normalize_run_id(const std::string &name);
    bool is_temp_run_dir(const std::string &name);
    std::string build_temp_run_dir(const std::string &final_dir);
}
