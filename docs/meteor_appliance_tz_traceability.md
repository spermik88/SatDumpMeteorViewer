# Meteor LRPT Appliance TZ Traceability

Date: 2026-02-23
Scope: Android appliance mode with RTL-SDR receiver, archive-only image output, 24/7 loop.

Status legend:
- `Implemented` means code path is present in repository.
- `Pending device test` means validation on Android hardware with RTL-SDR is still required.

| TZ item | File / method | Test case | Status |
|---|---|---|---|
| 24/7 no schedule/TLE/internet loop | `src-interface/recorder/recorder_proc.cpp` `tick_background()` `autostart_appliance_pipeline()` | A1 | Implemented, Pending device test |
| RTL source selection + keep alive | `src-interface/recorder/recorder_proc.cpp` `select_rtl_source_or_fail()` `handle_source_restart()` | A1, D1, D2 | Implemented, Pending device test |
| Backoff strictly `3 -> 60` | `src-interface/recorder/recorder_proc.cpp` (`restart_reset_backoff=3`, `restart_max_backoff=60`, no-source retry cap 60) | D1, D2 | Implemented, Pending device test |
| No-IQ/offline/error recovery triggers | `src-interface/recorder/recorder_proc.cpp` `handle_source_restart()` | D1, D2 | Implemented, Pending device test |
| Unified archive path for live + archive | `src-interface/archive_path.cpp` + uses in `main_ui.cpp` `processing.cpp` `viewer.cpp` `recorder.cpp` `recorder_proc.cpp` `recorder_vfo.cpp` | A3, A4, A5 | Implemented, Pending device test |
| Archive base dir creation on startup | `src-interface/main_ui.cpp` `initMainUI()` + `src-interface/recorder/recorder.cpp` ctor | A1 | Implemented, Pending device test |
| Disk limit 10 GiB FIFO on archive | `src-interface/processing.cpp` `enforce_images_disk_limit()` | A5 | Implemented, Pending device test |
| Pass end lifecycle (first valid frame + inactivity timeout) | `src-interface/recorder/recorder_proc.cpp` `maybe_emit_first_valid_frame()` `maybe_finalize_pass_on_inactivity()` | A2, A3 | Implemented, Pending device test |
| Auto finalize package + tmp->final | `src-interface/recorder/recorder_proc.cpp` `stop_processing()` + `finalize_live_output_dir()` | A3 | Implemented, Pending device test |
| Run packaging output set | `src-interface/processing.cpp` `package_run_output()` | A3 | Implemented, Pending device test |
| Image-only target format for new runs | `src-interface/processing.cpp` `prune_run_to_image_only()` (appliance path only) | A3, A4 | Implemented, Pending device test |
| Viewer default screen and newest run on startup | `src-interface/main_ui.cpp` `initMainUI()` + `open_run_in_viewer()` | A1, A4 | Implemented, Pending device test |
| First valid frame auto-open in Viewer | `src-interface/main_ui.cpp` `FirstValidFrameEvent` handler | A2 | Implemented, Pending device test |
| Archive grid + open run by tap | `src-interface/main_ui.cpp` archive UI + `open_run_in_viewer()` | A3, A4 | Implemented, Pending device test |
| Swipe left/right run navigation | `src-interface/viewer/viewer.cpp` `handleSwipePassNavigation()` `switchPass()` | A4, B4 | Implemented, Pending device test |
| Gestures: drag, double-tap, pinch | `src-interface/viewer/viewer.cpp` image widget interactions + `android/imgui_backends/imgui_impl_android.cpp` pinch wheel bridge | B1, B2, B3 | Implemented, Pending device test |
| Android system Back behavior | `android/main.cpp` `handleInputEvent()` (`Viewer->Archive` consume, `Archive->system`) | C1, C2 | Implemented, Pending device test |
| Bottom bar top row (RX/SDR/IMG/Back) | `src-interface/status_logger_sink.cpp` `draw()` | B5 | Implemented, Pending device test |
| Bottom bar layer row (SINGLE/STACK, preview, available layers only) | `src-interface/status_logger_sink.cpp` `draw_layer_bar()` + `src-interface/viewer/viewer.cpp` layer mode logic | B5 | Implemented, Pending device test |
| Image-only archive load without dataset dependency | `src-interface/viewer/viewer.cpp` `loadArchiveRun()` `loadArchiveImagesFromRun()` + `src-interface/main_ui.cpp` `open_run_in_viewer()` | A3, A4 | Implemented, Pending device test |
| Legacy run read compatibility | `src-interface/main_ui.cpp` legacy `dataset.json` fallback | A4 | Implemented, Pending device test |

## Device acceptance checklist

- [ ] A1 Start app offline/no TLE, SDR loop and statuses are correct.
- [ ] A2 Meteor signal appears, state goes waiting->receiving, first valid frame auto-opens Viewer.
- [ ] A3 Pass ends, run is finalized and appears in Archive with thumb and datetime.
- [ ] A4 Multiple runs are independent, swipe switches runs.
- [ ] A5 Archive >10 GiB removes oldest runs FIFO without UI errors.
- [ ] B1 Pinch zoom works smoothly on Android.
- [ ] B2 Drag pan works.
- [ ] B3 Double-tap resets fit-to-screen.
- [ ] B4 Swipe left/right switches passes.
- [ ] B5 Two-row bottom bar content is always present and correct.
- [ ] C1 Back in Viewer opens Archive.
- [ ] C2 Back in Archive exits by default Android behavior.
- [ ] D1 Hot unplug/replug RTL recovers with backoff `3 -> 60`.
- [ ] D2 No-IQ timeout and source error/offline trigger restart and status updates.
- [ ] D3 Long run (8-12h) stable: no lockups, no status flapping.
