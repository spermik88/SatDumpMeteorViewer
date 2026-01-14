#include "common/ops_state.h"
#include "core/plugin.h"
#include "logger.h"

#include <filesystem>
#include <mutex>

namespace satdump::ops
{
    namespace
    {
        std::mutex ops_state_mutex;
        OpsStateSnapshot ops_state;
        bool handlers_registered = false;
    }

    void register_event_handlers()
    {
        if (handlers_registered)
            return;
        handlers_registered = true;

        eventBus->register_handler<FirstValidFrameEvent>([](FirstValidFrameEvent evt)
                                                         {
                                                             std::string run_id = normalize_run_id(evt.run_id);
                                                             {
                                                                 std::lock_guard<std::mutex> lock(ops_state_mutex);
                                                                 ops_state.last_event = "first_valid_frame";
                                                                 if (ops_state.live_run_id.empty() || ops_state.live_run_id == run_id)
                                                                     ops_state.first_valid_frame = true;
                                                             }
                                                             logger->info("Event first_valid_frame: run_id=%s source=%s",
                                                                          run_id.c_str(),
                                                                          evt.source.c_str());
                                                         });

        eventBus->register_handler<RunFinalizedEvent>([](RunFinalizedEvent evt)
                                                      {
                                                          std::string run_id = normalize_run_id(evt.run_id);
                                                          {
                                                              std::lock_guard<std::mutex> lock(ops_state_mutex);
                                                              ops_state.last_event = "run_finalized";
                                                              ops_state.last_finalized_run_id = run_id;
                                                              if (ops_state.live_run_id.empty() || ops_state.live_run_id == run_id)
                                                              {
                                                                  ops_state.run_finalized = true;
                                                                  ops_state.pipeline_active = false;
                                                              }
                                                          }
                                                          logger->info("Event run_finalized: run_id=%s output_dir=%s",
                                                                       run_id.c_str(),
                                                                       evt.output_dir.c_str());
                                                      });

        eventBus->register_handler<FifoDeleteEvent>([](FifoDeleteEvent evt)
                                                    {
                                                        std::string run_id = normalize_run_id(evt.run_id);
                                                        {
                                                            std::lock_guard<std::mutex> lock(ops_state_mutex);
                                                            ops_state.last_event = "fifo_delete";
                                                            ops_state.last_deleted_run_id = run_id;
                                                            ops_state.fifo_delete = true;
                                                        }
                                                        logger->info("Event fifo_delete: run_id=%s output_dir=%s",
                                                                     run_id.c_str(),
                                                                     evt.output_dir.c_str());
                                                    });
    }

    OpsStateSnapshot get_state()
    {
        std::lock_guard<std::mutex> lock(ops_state_mutex);
        return ops_state;
    }

    void set_live_run(const std::string &run_id,
                      const std::string &tmp_dir,
                      const std::string &final_dir,
                      double start_timestamp)
    {
        std::lock_guard<std::mutex> lock(ops_state_mutex);
        ops_state.live_run_id = run_id;
        ops_state.live_tmp_dir = tmp_dir;
        ops_state.live_final_dir = final_dir;
        ops_state.live_start_timestamp = start_timestamp;
        ops_state.pipeline_active = true;
        ops_state.first_valid_frame = false;
        ops_state.run_finalized = false;
    }

    void set_pipeline_active(bool active)
    {
        std::lock_guard<std::mutex> lock(ops_state_mutex);
        ops_state.pipeline_active = active;
    }

    std::string normalize_run_id(const std::string &name)
    {
        constexpr const char *kPrefix = ".tmp_";
        if (name.rfind(kPrefix, 0) == 0)
            return name.substr(std::char_traits<char>::length(kPrefix));
        return name;
    }

    bool is_temp_run_dir(const std::string &name)
    {
        return name.rfind(".tmp_", 0) == 0;
    }

    std::string build_temp_run_dir(const std::string &final_dir)
    {
        std::filesystem::path final_path(final_dir);
        if (final_path.filename().empty())
            return final_dir;
        std::filesystem::path tmp_name = ".tmp_" + final_path.filename().string();
        return (final_path.parent_path() / tmp_name).string();
    }
}
