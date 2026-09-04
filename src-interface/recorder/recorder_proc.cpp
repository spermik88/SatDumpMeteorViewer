#include "recorder.h"

#include "main_ui.h"
#include "logger.h"
#include "processing.h"
#include "common/ops_state.h"
#include "core/plugin.h"
#include "products/dataset.h"
#include "products/image_products.h"
#include "products/products.h"
#include "archive_path.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <system_error>

#ifndef _MSC_VER
#include <sys/statvfs.h>
#endif

namespace satdump
{
    namespace
    {
        void set_status_env(const char *key, const std::string &value)
        {
#ifdef _WIN32
            _putenv_s(key, value.c_str());
#else
            setenv(key, value.c_str(), 1);
#endif
        }

        std::string remap_output_path(const std::string &path,
                                      const std::string &from_dir,
                                      const std::string &to_dir)
        {
            if (from_dir.empty() || to_dir.empty())
                return path;
            if (path.rfind(from_dir, 0) != 0)
                return path;
            std::filesystem::path rel = std::filesystem::path(path).lexically_relative(from_dir);
            if (rel.empty())
                return path;
            return (std::filesystem::path(to_dir) / rel).string();
        }

        bool is_android_permission_error(const std::error_code &ec)
        {
#ifdef __ANDROID__
            return ec == std::errc::permission_denied ||
                   ec == std::errc::operation_not_permitted ||
                   ec == std::errc::read_only_file_system;
#else
            (void)ec;
            return false;
#endif
        }

        bool prepare_live_output_dirs(std::string &final_dir,
                                      std::string &tmp_dir)
        {
            auto try_prepare = [&](const std::string &target_final_dir, std::error_code &out_ec) -> bool
            {
                tmp_dir = ops::build_temp_run_dir(target_final_dir);
                out_ec.clear();
                if (std::filesystem::exists(tmp_dir, out_ec))
                {
                    std::filesystem::remove_all(tmp_dir, out_ec);
                    if (out_ec)
                    {
                        logger->warn("Failed to clean temp directory %s: %s", tmp_dir.c_str(), out_ec.message().c_str());
                        return false;
                    }
                }
                std::filesystem::create_directories(tmp_dir, out_ec);
                if (out_ec)
                    return false;
                return true;
            };

            std::error_code ec;
            if (try_prepare(final_dir, ec))
                return true;

#ifdef __ANDROID__
            if (is_android_permission_error(ec))
            {
                std::filesystem::path requested_path(final_dir);
                std::string run_name = requested_path.filename().string();
                if (run_name.empty())
                    run_name = "run";

                std::string fallback_final_dir = (get_archive_base_path() / run_name).string();
                if (fallback_final_dir != final_dir)
                {
                    logger->warn("Live output path is not writable (%s), falling back to %s",
                                 final_dir.c_str(),
                                 fallback_final_dir.c_str());
                    final_dir = fallback_final_dir;
                    if (try_prepare(final_dir, ec))
                        return true;
                }
            }
#endif

            logger->error("Failed to create temp directory %s: %s", tmp_dir.c_str(), ec.message().c_str());
            return false;
        }

        bool finalize_live_output_dir(const std::string &tmp_dir,
                                      const std::string &final_dir)
        {
            if (tmp_dir.empty() || final_dir.empty() || tmp_dir == final_dir)
                return true;

            std::error_code ec;
            std::filesystem::rename(tmp_dir, final_dir, ec);
            if (ec)
            {
                logger->error("Failed to finalize run directory %s -> %s: %s",
                              tmp_dir.c_str(),
                              final_dir.c_str(),
                              ec.message().c_str());
                return false;
            }
            return true;
        }

        bool same_source_descriptor(const dsp::SourceDescriptor &a,
                                    const dsp::SourceDescriptor &b)
        {
            if (a.source_type != b.source_type)
                return false;

            if (!a.unique_id.empty() || !b.unique_id.empty())
                return a.unique_id == b.unique_id;

            return a.name == b.name;
        }

        int find_source_index(const std::vector<dsp::SourceDescriptor> &list,
                              const dsp::SourceDescriptor &target)
        {
            for (int i = 0; i < (int)list.size(); i++)
            {
                if (same_source_descriptor(list[i], target))
                    return i;
            }
            return -1;
        }

        bool same_source_lists(const std::vector<dsp::SourceDescriptor> &a,
                               const std::vector<dsp::SourceDescriptor> &b)
        {
            if (a.size() != b.size())
                return false;

            for (const auto &src : a)
            {
                if (find_source_index(b, src) < 0)
                    return false;
            }
            return true;
        }

        std::string make_source_select_string(const std::vector<dsp::SourceDescriptor> &sources)
        {
            std::string out;
            for (const auto &src : sources)
                out += src.name + '\0';
            return out;
        }

        struct RunArtifactProbe
        {
            std::filesystem::file_time_type latest_mtime = std::filesystem::file_time_type::min();
            size_t file_count = 0;
        };

        RunArtifactProbe collect_run_artifact_probe(const std::filesystem::path &run_dir)
        {
            RunArtifactProbe out;
            if (run_dir.empty() || !std::filesystem::exists(run_dir))
                return out;

            std::error_code ec;
            auto it = std::filesystem::recursive_directory_iterator(
                run_dir,
                std::filesystem::directory_options::skip_permission_denied,
                ec);
            auto end = std::filesystem::recursive_directory_iterator();
            while (it != end)
            {
                if (it->is_regular_file(ec))
                {
                    out.file_count++;
                    std::error_code ts_ec;
                    auto ts = std::filesystem::last_write_time(it->path(), ts_ec);
                    if (!ts_ec && (out.latest_mtime == std::filesystem::file_time_type::min() || ts > out.latest_mtime))
                        out.latest_mtime = ts;
                }
                it.increment(ec);
            }

            return out;
        }
    }

    nlohmann::json RecorderApplication::serialize_config()
    {
        nlohmann::json out;
        out["show_waterfall"] = show_waterfall;
        out["waterfall_ratio"] = waterfall_ratio;
        out["panel_ratio"] = panel_ratio;
        out["fft_size"] = fft_size;
        out["fft_rate"] = fft_rate;
        out["waterfall_rate"] = waterfall_rate;
        out["waterfall_palette"] = waterfall_palettes[selected_waterfall_palette].name;
        out["baseband_type"] = (std::string)baseband_format;
        if (fft_plot && waterfall_plot && fft)
        {
            out["fft_min"] = fft_plot->scale_min;
            out["fft_max"] = fft_plot->scale_max;
            out["fft_avgn"] = fft->avg_num;
        }

#if defined(BUILD_ZIQ) || defined(BUILD_ZIQ2)
        out["ziq_depth"] = baseband_format.ziq_depth;
#endif
        return out;
    }

    void RecorderApplication::deserialize_config(nlohmann::json in)
    {
        show_waterfall = in["show_waterfall"].get<bool>();
        waterfall_ratio = in["waterfall_ratio"].get<float>();
        panel_ratio = in["panel_ratio"].get<float>();
        if (fft_plot && waterfall_plot && fft)
        {
            if (in.contains("fft_min"))
                fft_plot->scale_min = in["fft_min"];
            if (in.contains("fft_max"))
                fft_plot->scale_max = in["fft_max"];
            if (in.contains("fft_avgn"))
                fft->avg_num = in["fft_avgn"];
        }
        if (in.contains("fft_size"))
        {
            fft_size = in["fft_size"].get<int>();
            for (int i = 0; i < (int)fft_sizes_lut.size(); i++)
                if (fft_sizes_lut[i] == fft_size)
                    selected_fft_size = i;
        }
        if (in.contains("fft_rate"))
            fft_rate = in["fft_rate"];
        if (in.contains("waterfall_rate"))
            waterfall_rate = in["waterfall_rate"];
        if (in.contains("baseband_type"))
            baseband_format = in["baseband_type"].get<std::string>();
        if (in.contains("waterfall_palette"))
        {
            std::string name = in["waterfall_palette"].get<std::string>();
            for (int i = 0; i < (int)waterfall_palettes.size(); i++)
                if (waterfall_palettes[i].name == name)
                    selected_waterfall_palette = i;
            waterfall_plot->set_palette(waterfall_palettes[selected_waterfall_palette]);
        }
#if defined(BUILD_ZIQ) || defined(BUILD_ZIQ2)
        if (in.contains("ziq_depth"))
            baseband_format.ziq_depth = in["ziq_depth"];
#endif
    }

    void RecorderApplication::start()
    {
        if (is_started)
            return;
        if (!source_ptr)
        {
            set_sdr_status("offline");
            if (appliance_mode)
                set_rx_status("waiting");
            return;
        }

        set_frequency(frequency_hz);

        try
        {
            current_samplerate = source_ptr->get_samplerate();
            if (current_samplerate == 0)
                throw satdump_exception("Samplerate not set!");

            source_ptr->start();
            source_ptr->set_status(dsp::DSPSampleSource::SourceStatus::Online);

            if (current_decimation > 1)
            {
                decim_ptr = std::make_shared<dsp::SmartResamplerBlock<complex_t>>(source_ptr->output_stream, 1, current_decimation);
                decim_ptr->start();
                logger->info("Setting up resampler...");
            }

            fft->set_fft_settings(fft_size, get_samplerate(), fft_rate);
            waterfall_plot->set_rate(fft_rate, waterfall_rate);
            fft_plot->bandwidth = get_samplerate();

            splitter->input_stream = current_decimation > 1 ? decim_ptr->output_stream : source_ptr->output_stream;
            splitter->start();
            is_started = true;
            set_sdr_status("online");
        }
        catch (std::runtime_error &e)
        {
            source_ptr->set_status(dsp::DSPSampleSource::SourceStatus::Error);
            sdr_error.set_message(style::theme.red, e.what());
            logger->error(e.what());
            set_sdr_status("error");
        }
    }

    void RecorderApplication::stop()
    {
        if (!is_started)
            return;
        if (!source_ptr)
            return;

        splitter->stop_tmp();
        if (current_decimation > 1)
            decim_ptr->stop();
        source_ptr->stop();
        is_started = false;
        source_ptr->set_status(dsp::DSPSampleSource::SourceStatus::Offline);
        set_sdr_status("offline");
        if (sdr_select_id >= 0 && sdr_select_id < (int)sources.size())
        {
            config::main_cfg["user"]["recorder_sdr_settings"]["last_used_sdr"] = sources[sdr_select_id].name;
            config::main_cfg["user"]["recorder_sdr_settings"][sources[sdr_select_id].name] = source_ptr->get_settings();
            config::main_cfg["user"]["recorder_sdr_settings"][sources[sdr_select_id].name]["samplerate"] = source_ptr->get_samplerate();
            config::main_cfg["user"]["recorder_sdr_settings"][sources[sdr_select_id].name]["frequency"] = frequency_hz;
            config::main_cfg["user"]["recorder_sdr_settings"][sources[sdr_select_id].name]["xconverter_frequency"] = xconverter_frequency;
            config::main_cfg["user"]["recorder_sdr_settings"][sources[sdr_select_id].name]["decimation"] = current_decimation;
            config::saveUserConfig();
        }
    }

    bool RecorderApplication::is_meteor_pipeline_active() const
    {
        if (!is_processing || !live_pipeline)
            return false;

        std::string name = pipeline_selector.selected_pipeline.name;
        std::string readable = pipeline_selector.selected_pipeline.readable_name;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });
        std::transform(readable.begin(), readable.end(), readable.begin(), [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });

        return (name.rfind("meteor_", 0) == 0) || (readable.find("meteor") != std::string::npos);
    }

    bool RecorderApplication::is_rtl_source_descriptor(const dsp::SourceDescriptor &src) const
    {
        std::string key = src.source_type + " " + src.name + " " + src.unique_id;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });
        return key.find("rtlsdr") != std::string::npos ||
               key.find("rtl2832") != std::string::npos ||
               key.find("rtl-sdr") != std::string::npos ||
               key.find(" rtl ") != std::string::npos ||
               key.rfind("rtl ", 0) == 0;
    }

    bool RecorderApplication::has_first_valid_frame_artifact(const std::string &run_dir) const
    {
        if (run_dir.empty())
            return false;

        std::filesystem::path dataset_path = std::filesystem::path(run_dir) / "dataset.json";
        if (!std::filesystem::exists(dataset_path))
            return false;

        ProductDataSet dataset;
        try
        {
            dataset.load(dataset_path.string());
        }
        catch (const std::exception &)
        {
            return false;
        }

        if (dataset.products_list.empty())
            return false;

        for (const auto &product_entry : dataset.products_list)
        {
            std::filesystem::path product_path = product_entry;
            if (product_path.is_relative())
                product_path = std::filesystem::path(run_dir) / product_path;

            try
            {
                std::shared_ptr<Products> products = loadProducts(product_path.string());
                auto image_products = std::dynamic_pointer_cast<ImageProducts>(products);
                if (!image_products)
                    continue;

                for (const auto &img_holder : image_products->images)
                {
                    if (img_holder.image.width() > 0 && img_holder.image.height() > 0)
                        return true;
                }
            }
            catch (const std::exception &)
            {
            }
        }

        return false;
    }

    void RecorderApplication::maybe_emit_first_valid_frame()
    {
        if (!is_processing || pipeline_run_id.empty())
            return;
        if (first_valid_frame_emitted_run_id == pipeline_run_id)
            return;

        auto now = std::chrono::steady_clock::now();
        if (now < next_first_valid_probe_time)
            return;
        next_first_valid_probe_time = now + std::chrono::seconds(1);

        std::string probe_dir = pipeline_output_dir_tmp.empty() ? pipeline_output_dir : pipeline_output_dir_tmp;
        if (!has_first_valid_frame_artifact(probe_dir))
            return;

        eventBus->fire_event<ops::FirstValidFrameEvent>({pipeline_run_id, "live_dataset"});
        first_valid_frame_emitted_run_id = pipeline_run_id;
        pass_last_artifact_update_time = now;
        set_rx_status("receiving");
    }

    void RecorderApplication::reset_pass_inactivity_watchdog()
    {
        next_pass_inactivity_probe_time = std::chrono::steady_clock::time_point::min();
        pass_last_artifact_update_time = std::chrono::steady_clock::time_point::min();
        pass_last_artifact_mtime = std::filesystem::file_time_type::min();
        pass_last_artifact_file_count = 0;
    }

    void RecorderApplication::maybe_finalize_pass_on_inactivity()
    {
        if (!appliance_mode || !is_processing || pipeline_run_id.empty())
            return;

        // Auto-finalize only after the pass has produced the first valid frame.
        if (first_valid_frame_emitted_run_id != pipeline_run_id && !ops::get_state().first_valid_frame)
            return;

        auto now = std::chrono::steady_clock::now();
        if (now < next_pass_inactivity_probe_time)
            return;
        next_pass_inactivity_probe_time = now + std::chrono::seconds(1);

        std::filesystem::path probe_dir = pipeline_output_dir_tmp.empty() ? pipeline_output_dir : pipeline_output_dir_tmp;
        RunArtifactProbe probe = collect_run_artifact_probe(probe_dir);

        bool changed = false;
        if (probe.file_count != pass_last_artifact_file_count)
            changed = true;
        if (probe.latest_mtime != std::filesystem::file_time_type::min() &&
            (pass_last_artifact_mtime == std::filesystem::file_time_type::min() || probe.latest_mtime > pass_last_artifact_mtime))
            changed = true;

        pass_last_artifact_file_count = probe.file_count;
        pass_last_artifact_mtime = probe.latest_mtime;
        if (changed)
            pass_last_artifact_update_time = now;

        if (pass_last_artifact_update_time == std::chrono::steady_clock::time_point::min())
            pass_last_artifact_update_time = now;

        auto inactive_for = std::chrono::duration_cast<std::chrono::seconds>(now - pass_last_artifact_update_time).count();
        if (inactive_for < pass_inactivity_timeout_seconds)
            return;

        logger->info("No updates in run artifacts for %lld seconds, finalizing pass %s.",
                     (long long)inactive_for,
                     pipeline_run_id.c_str());
        stop_processing();
    }

    void RecorderApplication::set_sdr_status(const std::string &status)
    {
        if (sdr_status == status)
            return;

        sdr_status = status;
        set_status_env("SDR_STATUS", status);
        if (status == "online")
            ops::set_sdr_stage(ops::SdrStage::Online);
        else if (status == "error")
            ops::set_sdr_stage(ops::SdrStage::Error);
        else
            ops::set_sdr_stage(ops::SdrStage::Offline);
    }

    void RecorderApplication::set_rx_status(const std::string &status)
    {
        if (rx_status == status)
            return;

        rx_status = status;
        set_status_env("RX_STATUS", status);
        if (status == "waiting")
            ops::set_rx_stage(ops::RxStage::WaitingSignal);
        else if (status == "receiving")
            ops::set_rx_stage(ops::RxStage::Receiving);
        else if (status == "decoding")
            ops::set_rx_stage(ops::RxStage::Decoding);
        else if (status == "error")
            ops::set_rx_stage(ops::RxStage::Error);
        else
            ops::set_rx_stage(ops::RxStage::Idle);
    }

    void RecorderApplication::handle_source_restart()
    {
        const int restart_reset_backoff = 3;
        const int restart_max_backoff = 60;

        if (!source_ptr)
        {
            if (appliance_mode)
            {
                set_rx_status("waiting");
                select_rtl_source_or_fail();
            }
            return;
        }

        auto status = source_ptr->get_status();
        bool sdr_online = status == dsp::DSPSampleSource::SourceStatus::Online;
        bool no_iq_timeout = false;
        if (is_started && splitter)
        {
            double no_iq_seconds = splitter->seconds_since_last_input();
            no_iq_timeout = no_iq_seconds > 5.0;
        }

        if (sdr_online && !no_iq_timeout)
        {
            if (is_processing)
                set_rx_status(ops::get_state().first_valid_frame ? "receiving" : "waiting");
            else
                set_rx_status(appliance_mode ? "waiting" : "idle");

            set_sdr_status("online");
            source_restart_pending = false;
            pipeline_restart_pending = false;
            source_restart_backoff_seconds = restart_reset_backoff;
            return;
        }

        auto now = std::chrono::steady_clock::now();
        if (!source_restart_pending)
        {
            if (no_iq_timeout)
            {
                logger->warn("No IQ data detected for over 5 seconds, restarting...");
                set_rx_status("waiting");
            }
            else
            {
                logger->warn("SDR source is offline/error, restarting...");
                if (status == dsp::DSPSampleSource::SourceStatus::Offline)
                    set_sdr_status("offline");
                else if (status == dsp::DSPSampleSource::SourceStatus::Error)
                    set_sdr_status("error");
            }

            if (is_started)
                stop();
            if (is_meteor_pipeline_active())
            {
                pipeline_restart_pending = true;
                stop_processing();
                set_rx_status("waiting");
            }

            source_ptr->close();
            source_restart_pending = true;
            set_sdr_status("offline");
            source_restart_time = now + std::chrono::seconds(source_restart_backoff_seconds);
            return;
        }

        if (now < source_restart_time)
            return;

        try
        {
            source_ptr->open();
            start();
            if (source_ptr->get_status() == dsp::DSPSampleSource::SourceStatus::Online)
            {
                source_restart_pending = false;
                source_restart_backoff_seconds = restart_reset_backoff;
                set_sdr_status("online");
                if (pipeline_restart_pending)
                {
                    pipeline_restart_pending = false;
                    start_processing();
                }
                return;
            }
        }
        catch (std::exception &e)
        {
            source_ptr->set_status(dsp::DSPSampleSource::SourceStatus::Error);
            sdr_error.set_message(style::theme.red, e.what());
            logger->error("Failed to restart SDR source: %s", e.what());
            set_sdr_status("error");
        }

        source_restart_backoff_seconds = std::min(source_restart_backoff_seconds * 2, restart_max_backoff);
        source_restart_time = now + std::chrono::seconds(source_restart_backoff_seconds);
        set_sdr_status("offline");
    }

    void RecorderApplication::try_load_sdr_settings()
    {
        if (!source_ptr || sdr_select_id < 0 || sdr_select_id >= (int)sources.size())
            return;

        auto &saved_settings = config::main_cfg["user"]["recorder_sdr_settings"];
        auto &cfg = saved_settings[sources[sdr_select_id].name];

        // A very low manual gain was inherited from the desktop-style recorder
        // and made the Android Meteor appliance effectively deaf. Migrate that
        // value once, while preserving an intentional gain selected afterwards.
        constexpr const char *meteor_gain_migration_key = "meteor_appliance_gain_v1";
        if (appliance_mode &&
            is_rtl_source_descriptor(sources[sdr_select_id]) &&
            !getValueOrDefault(cfg[meteor_gain_migration_key], false))
        {
            float saved_gain = getValueOrDefault(cfg["gain"], 0.0f);
            if (!cfg.contains("gain") || saved_gain < 10.0f)
            {
                cfg["gain"] = 40.2f;
                cfg["lna_agc"] = false;
                cfg["tuner_agc"] = false;
                logger->info("Using Android Meteor RTL-SDR gain default: 40.2 dB");
            }
            cfg[meteor_gain_migration_key] = true;
            config::saveUserConfig();
        }

        source_ptr->set_settings(cfg);
        if (cfg.contains("samplerate"))
        {
            try
            {
                source_ptr->set_samplerate(cfg["samplerate"]);
            }
            catch (std::exception &)
            {
            }
        }
        if (cfg.contains("frequency"))
        {
            frequency_hz = cfg["frequency"].get<uint64_t>();
            set_frequency(frequency_hz);
        }
        if (cfg.contains("xconverter_frequency"))
            xconverter_frequency = cfg["xconverter_frequency"].get<double>();
        else
            xconverter_frequency = 0;
        if (cfg.contains("decimation"))
            current_decimation = cfg["decimation"].get<int>();
        else
            current_decimation = 1;
    }

    void RecorderApplication::start_processing()
    {
        if (pipeline_selector.outputdirselect.isValid() || automated_live_output_dir)
        {
            logger->trace("Start pipeline...");
            pipeline_params = pipeline_selector.getParameters();
            pipeline_params["samplerate"] = get_samplerate();
            pipeline_params["baseband_format"] = "cf32";
            pipeline_params["buffer_size"] = dsp::STREAM_BUFFER_SIZE; // This is required, as we WILL go over the (usually) default 8192 size
            pipeline_params["start_timestamp"] = (double)time(0);     // Some pipelines need this

            try
            {
                if (automated_live_output_dir)
                {
                    if (appliance_mode)
                        ensure_archive_base_path();
                    pipeline_output_dir = prepareAutomatedPipelineFolder(time(0),
                                                                         source_ptr->d_frequency,
                                                                         pipeline_selector.selected_pipeline.name,
                                                                         appliance_mode ? get_archive_base_path().string() : "",
                                                                         false);
                }
                else
                    pipeline_output_dir = pipeline_selector.outputdirselect.getPath();

                if (!prepare_live_output_dirs(pipeline_output_dir, pipeline_output_dir_tmp))
                    throw std::runtime_error("Failed to prepare live output directory");

                pipeline_run_id = ops::normalize_run_id(std::filesystem::path(pipeline_output_dir).filename().string());
                first_valid_frame_emitted_run_id.clear();
                next_first_valid_probe_time = std::chrono::steady_clock::time_point::min();
                reset_pass_inactivity_watchdog();
                ops::set_live_run(pipeline_run_id,
                                  pipeline_output_dir_tmp,
                                  pipeline_output_dir,
                                  pipeline_params["start_timestamp"].get<double>());

                live_pipeline = std::make_unique<LivePipeline>(pipeline_selector.selected_pipeline, pipeline_params, pipeline_output_dir_tmp);
                splitter->reset_output("live");
                live_pipeline->start(splitter->get_output("live"), ui_thread_pool);
                splitter->set_enabled("live", true);

                is_processing = true;
                set_rx_status("waiting");
            }
            catch (std::runtime_error &e)
            {
                error.set_message(style::theme.red, e.what());
                logger->error(e.what());
                ops::set_pipeline_active(false);
                set_rx_status("error");
            }
        }
        else
        {
            error.set_message(style::theme.red, "Please select a valid output directory!");
            set_rx_status("error");
        }
    }

    void RecorderApplication::stop_processing()
    {
        if (is_processing)
        {
            set_rx_status("decoding");
            is_stopping_processing = true;
            logger->trace("Stop pipeline...");
            splitter->set_enabled("live", false);
            live_pipeline->stop();
            is_stopping_processing = is_processing = false;

            std::vector<std::string> output_files = live_pipeline->getOutputFiles();

            // Disconnecting an SDR before a valid Meteor frame used to turn the
            // empty CADU into a final archive and start post-processing it. Keep
            // the appliance waiting for the receiver instead.
            if (appliance_mode && !ops::get_state().first_valid_frame)
            {
                live_pipeline.reset();
                std::error_code remove_ec;
                std::filesystem::path tmp_path(pipeline_output_dir_tmp);
                if (!tmp_path.empty() && ops::is_temp_run_dir(tmp_path.filename().string()))
                    std::filesystem::remove_all(tmp_path, remove_ec);
                if (remove_ec)
                    logger->warn("Failed to discard empty Meteor run %s: %s",
                                 pipeline_output_dir_tmp.c_str(),
                                 remove_ec.message().c_str());
                else
                    logger->info("Discarded Meteor run without a valid frame: %s", pipeline_run_id.c_str());

                ops::set_pipeline_active(false);
                reset_pass_inactivity_watchdog();
                set_rx_status("waiting");
                return;
            }

            bool finalized = finalize_live_output_dir(pipeline_output_dir_tmp, pipeline_output_dir);
            std::string output_dir_for_processing = finalized ? pipeline_output_dir : pipeline_output_dir_tmp;

            bool launched_postproc = false;
            if (config::main_cfg["user_interface"]["finish_processing_after_live"]["value"].get<bool>() && !output_files.empty())
            {
                Pipeline pipeline = pipeline_selector.selected_pipeline;
                std::string input_file = remap_output_path(output_files[0], pipeline_output_dir_tmp, output_dir_for_processing);
                int start_level = pipeline.live_cfg.normal_live[pipeline.live_cfg.normal_live.size() - 1].first;
                std::string input_level = pipeline.steps[start_level].level_name;
                ui_thread_pool.push([=](int)
                                    { processing::process(pipeline, input_level, input_file, output_dir_for_processing, pipeline_params); });
                launched_postproc = true;
            }

            live_pipeline.reset();
            bool prune_to_image_only = appliance_mode && !launched_postproc;
            processing::package_run_output(output_dir_for_processing, pipeline_run_id, prune_to_image_only);
            if (finalized)
                eventBus->fire_event<ops::RunFinalizedEvent>({pipeline_run_id, pipeline_output_dir});
            if (!launched_postproc)
                processing::enforce_images_disk_limit(output_dir_for_processing);
            ops::set_pipeline_active(false);
            reset_pass_inactivity_watchdog();
            if (!launched_postproc)
                set_rx_status(appliance_mode ? "waiting" : "idle");
        }
    }

    bool RecorderApplication::select_rtl_source_or_fail()
    {
        if (appliance_mode)
        {
            std::vector<dsp::SourceDescriptor> fresh_sources = dsp::getAllAvailableSources();
            std::vector<dsp::SourceDescriptor> rtl_sources;
            rtl_sources.reserve(fresh_sources.size());
            for (const auto &src : fresh_sources)
            {
                if (is_rtl_source_descriptor(src))
                    rtl_sources.push_back(src);
            }
            sources = rtl_sources;
            sdr_select_string = make_source_select_string(sources);
        }

        if (sources.empty())
        {
            sdr_select_id = -1;
            set_sdr_status("offline");
            return false;
        }

        int start_index = 0;
        if (sdr_select_id >= 0 && sdr_select_id < (int)sources.size())
            start_index = sdr_select_id;

        for (int offset = 0; offset < (int)sources.size(); offset++)
        {
            int i = (start_index + offset) % (int)sources.size();
            try
            {
                source_ptr = dsp::getSourceFromDescriptor(sources[i]);
                source_ptr->open();
                sdr_select_id = i;
                try_load_sdr_settings();
                set_sdr_status("online");
                source_restart_pending = false;
                source_restart_backoff_seconds = 3;
                source_unhealthy_since = std::chrono::steady_clock::time_point::min();
                return true;
            }
            catch (std::runtime_error &e)
            {
                logger->error(e.what());
                source_ptr.reset();
            }
        }

        sdr_select_id = -1;
        set_sdr_status("offline");
        return false;
    }

    void RecorderApplication::autostart_appliance_pipeline()
    {
        if (!appliance_mode)
            return;

        if (!source_ptr && !select_rtl_source_or_fail())
            return;

        if (!is_started)
            start();

        if (!is_started)
            return;

        pipeline_selector.select_pipeline("meteor_m2-x_lrpt");
        if (!is_processing)
            start_processing();
    }

    void RecorderApplication::tick_background()
    {
        if (appliance_mode)
        {
            auto now = std::chrono::steady_clock::now();

            auto reset_appliance_source = [this]()
            {
                if (is_processing)
                    stop_processing();
                if (is_started)
                    stop();
                if (source_ptr)
                    source_ptr->close();

                source_ptr.reset();
                source_restart_pending = false;
                pipeline_restart_pending = false;
                source_restart_backoff_seconds = 3;
                source_unhealthy_since = std::chrono::steady_clock::time_point::min();
                reset_pass_inactivity_watchdog();
                set_sdr_status("offline");
                set_rx_status("waiting");
            };

            if (now >= next_rtl_rescan_time)
            {
                next_rtl_rescan_time = now + std::chrono::seconds(appliance_rescan_interval_seconds);

                std::vector<dsp::SourceDescriptor> fresh_sources = dsp::getAllAvailableSources();
                std::vector<dsp::SourceDescriptor> rtl_sources;
                rtl_sources.reserve(fresh_sources.size());
                for (const auto &src : fresh_sources)
                {
                    if (is_rtl_source_descriptor(src))
                        rtl_sources.push_back(src);
                }

                dsp::SourceDescriptor selected_before_rescan;
                bool had_selected = sdr_select_id >= 0 && sdr_select_id < (int)sources.size();
                if (had_selected)
                    selected_before_rescan = sources[sdr_select_id];

                bool list_changed = !same_source_lists(sources, rtl_sources);
                if (list_changed)
                {
                    sources = rtl_sources;
                    sdr_select_string = make_source_select_string(sources);
                    if (had_selected)
                        sdr_select_id = find_source_index(sources, selected_before_rescan);
                    else if (sources.empty())
                        sdr_select_id = -1;
                    else if (sdr_select_id >= (int)sources.size())
                        sdr_select_id = 0;

                    if (source_ptr && (sdr_select_id < 0 || sdr_select_id >= (int)sources.size()))
                    {
                        logger->warn("Current RTL source is no longer present, forcing reselect.");
                        reset_appliance_source();
                    }
                }
            }

            if (source_ptr)
            {
                auto status = source_ptr->get_status();
                bool unhealthy = status == dsp::DSPSampleSource::SourceStatus::Offline ||
                                 status == dsp::DSPSampleSource::SourceStatus::Error;
                if (unhealthy)
                {
                    if (source_unhealthy_since == std::chrono::steady_clock::time_point::min())
                        source_unhealthy_since = now;

                    auto unhealthy_for = std::chrono::duration_cast<std::chrono::seconds>(now - source_unhealthy_since).count();
                    if (unhealthy_for >= appliance_unhealthy_timeout_seconds)
                    {
                        logger->warn("RTL source stayed offline/error for %lld seconds, forcing reselect.", (long long)unhealthy_for);
                        reset_appliance_source();
                    }
                }
                else
                {
                    source_unhealthy_since = std::chrono::steady_clock::time_point::min();
                }
            }

            if (!source_ptr)
            {
                if (source_restart_pending && now < source_restart_time)
                    return;

                if (!select_rtl_source_or_fail())
                {
                    set_rx_status("waiting");
                    source_restart_pending = true;
                    source_restart_time = now + std::chrono::seconds(source_restart_backoff_seconds);
                    source_restart_backoff_seconds = std::min(source_restart_backoff_seconds * 2, 60);
                    return;
                }
            }

            if (source_ptr && !is_started && !source_restart_pending)
                start();
            if (source_ptr && is_started && !is_processing)
                autostart_appliance_pipeline();
        }

        maybe_emit_first_valid_frame();
        maybe_finalize_pass_on_inactivity();

        handle_source_restart();
    }

    void RecorderApplication::start_recording()
    {
        splitter->set_enabled("record", true);
        load_rec_path_data();
        std::string filename = recording_path + prepareBasebandFileName(getTime(), get_samplerate(), frequency_hz);
        recorder_filename = file_sink->start_recording(filename, get_samplerate());
        logger->info("Recording to " + recorder_filename);
        is_recording = true;
    }

    void RecorderApplication::stop_recording()
    {
        if (is_recording)
        {
            file_sink->stop_recording();
            splitter->set_enabled("record", false);
            recorder_filename = "";
            is_recording = false;
            load_rec_path_data();
        }
    }

    void RecorderApplication::load_rec_path_data()
    {
        recording_path = config::main_cfg["satdump_directories"]["recording_path"]["value"].get<std::string>();
#if defined(_MSC_VER)
        recording_path += "\\";
#elif defined(__ANDROID__)
        if (recording_path == ".")
            recording_path = "/storage/emulated/0";
        recording_path += "/";
#else
        recording_path += "/";
#endif

#ifdef _MSC_VER
        ULARGE_INTEGER bytes_available;
        if (GetDiskFreeSpaceEx(recording_path.c_str(), &bytes_available, NULL, NULL))
            disk_available = bytes_available.QuadPart;
#else
        struct statvfs stat_buffer;
        if (statvfs(recording_path.c_str(), &stat_buffer) == 0)
            disk_available = stat_buffer.f_bavail * stat_buffer.f_bsize;
#endif
    }

    void RecorderApplication::try_init_tracking_widget()
    {
        if (tracking_widget == nullptr)
        {
            tracking_widget = new TrackingWidget();

            tracking_widget->aos_callback = [this](AutoTrackCfg autotrack_cfg, SatellitePass, TrackedObject obj)
            {
                if (autotrack_cfg.multi_mode || obj.downlinks.size() > 1)
                {
                    if (!autotrack_cfg.multi_mode)
                    {
                        double center_freq = 0;
                        for (auto &dl : obj.downlinks)
                            center_freq += dl.frequency;
                        center_freq /= obj.downlinks.size();
                        set_frequency(center_freq);
                    }

                    for (auto &dl : obj.downlinks)
                    {
                        if (dl.live || dl.record)
                            if (!is_started)
                                start();

                        if (dl.live)
                        {
                            std::string id = std::to_string(obj.norad) + "_" + std::to_string(dl.frequency) + "_live";
                            std::string name = std::to_string(obj.norad);
                            std::optional<TLE> this_tle = satdump::general_tle_registry->get_from_norad(obj.norad);
                            if (this_tle.has_value())
                                name = this_tle->name;
                            name += " - " + format_notated(dl.frequency, "Hz");
                            add_vfo_live(id, name, dl.frequency, dl.pipeline_selector->selected_pipeline, dl.pipeline_selector->getParameters());
                        }

                        if (dl.record)
                        {
                            std::string id = std::to_string(obj.norad) + "_" + std::to_string(dl.frequency) + "_record";
                            std::string name = std::to_string(obj.norad);
                            std::optional<TLE> this_tle = satdump::general_tle_registry->get_from_norad(obj.norad);
                            if (this_tle.has_value())
                                name = this_tle->name;
                            name += " - " + format_notated(dl.frequency, "Hz");
                            add_vfo_reco(id, name, dl.frequency, dl.baseband_format, dl.baseband_decimation);
                        }
                    }
                }
                else
                {
                    if (obj.downlinks[0].live)
                        stop_processing();
                    if (obj.downlinks[0].record)
                        stop_recording();

                    if (obj.downlinks[0].live || obj.downlinks[0].record)
                    {
                        frequency_hz = obj.downlinks[0].frequency;
                        if (is_started)
                            set_frequency(frequency_hz);
                        else
                            start();

                        // Catch situations where source could not start
                        if (!is_started)
                        {
                            logger->error("Could not start recorder/processor since the source could not be started!");
                            return;
                        }
                    }

                    if (obj.downlinks[0].live)
                    {
                        pipeline_selector.select_pipeline(obj.downlinks[0].pipeline_selector->selected_pipeline.name);
                        pipeline_selector.setParameters(obj.downlinks[0].pipeline_selector->getParameters());
                        pipeline_selector.selected_pipeline.steps = obj.downlinks[0].pipeline_selector->selected_pipeline.steps;
                        start_processing();
                    }

                    if (obj.downlinks[0].record)
                    {
                        file_sink->set_output_sample_type(obj.downlinks[0].baseband_format);
                        start_recording();
                    }
                }
            };

            tracking_widget->los_callback = [this](AutoTrackCfg autotrack_cfg, SatellitePass, TrackedObject obj)
            {
                if (autotrack_cfg.multi_mode || obj.downlinks.size() > 1)
                {
                    for (auto &dl : obj.downlinks)
                    {
                        if (dl.live)
                        {
                            std::string id = std::to_string(obj.norad) + "_" + std::to_string(dl.frequency) + "_live";
                            del_vfo(id);
                        }

                        if (dl.record)
                        {
                            std::string id = std::to_string(obj.norad) + "_" + std::to_string(dl.frequency) + "_record";
                            del_vfo(id);
                        }

                        if (dl.live || dl.record)
                            if (is_started && vfo_list.size() == 0 && autotrack_cfg.stop_sdr_when_idle)
                                stop();
                    }
                }
                else
                {
                    if (obj.downlinks[0].record)
                        stop_recording();
                    if (obj.downlinks[0].live)
                        stop_processing();
                    if (autotrack_cfg.stop_sdr_when_idle)
                        stop();
                }
            };
        }
    }
}
