#include "hardwarescope/presentmon_fps_runner.hpp"
#include "hardwarescope/game_detector.hpp"
#include "hardwarescope/osd_model.hpp"
#include "hardwarescope/osd_window.hpp"
#include "hardwarescope/sensor_bridge.hpp"

#include <windows.h>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <memory>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2; // Explicit approved PID, output CSV path.
    const auto expected_pid = static_cast<DWORD>(wcstoul(argv[1], nullptr, 10));
    if (expected_pid == 0U) return 2;
    std::ofstream log{std::filesystem::path{argv[2]}};
    if (!log) return 2;
    const auto game = hardwarescope::FindGameProcess(GetCurrentProcessId());
    log << "detected_pid," << game.process_id << ",expected_pid," << expected_pid << '\n';
    if (game.process_id != expected_pid) { log << "FAIL game detection\n"; return 1; }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    auto runner = std::make_unique<hardwarescope::PresentMonFpsRunner>();
    auto osd = std::make_unique<hardwarescope::OsdWindow>(GetModuleHandleW(nullptr));
    hardwarescope::AppSettings settings;
    settings.easy_temperature_enabled = false;
    settings.osd_position = hardwarescope::OsdPosition::bottom_right;
    settings.fps_color_rgb = 0xFFFFFF;
    settings.fps_scale_percent = 150;
    settings.automatic_updates = false;
    settings.Normalize();
    if (!osd->Initialize(nullptr, settings)) { log << "FAIL OSD initialize\n"; return 1; }
    auto snapshot = std::make_unique<hardwarescope::SensorSnapshot>();
    auto installed = std::make_unique<hardwarescope::SensorSnapshot>();
    hardwarescope::SensorBridgeClient installed_bridge; // Read only, no SetFpsTarget.
    runner->SetTarget(expected_pid, 500U);
    const auto started = GetTickCount64();
    unsigned available{}, low_available{}, osd_mismatches{}, bridge_available{}, unavailable_after_start{};
    log << "elapsed_ms,available,fps,one_percent_low,frame_ms,osd_items,installed_fps,frame_qpc,low_frame_qpc,low_interval_count,receipt_qpc\n";
    for (;;) {
        const auto elapsed = GetTickCount64() - started;
        if (elapsed >= 90000U) break;
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        runner->SetTarget(expected_pid, 500U); // Same keep-alive path as the service.
        const auto reading = runner->Snapshot();
        if (!reading.available && available != 0U) ++unavailable_after_start;
        hardwarescope::ResetSnapshot(*snapshot);
        snapshot->sequence = elapsed + 1U;
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        snapshot->captured_qpc = static_cast<std::uint64_t>(qpc.QuadPart);
        if (reading.available) {
            ++available;
            const auto add = [&](std::uint64_t id, double value) {
                auto& sensor = snapshot->sensors[snapshot->count++];
                sensor = {};
                sensor.id = id;
                sensor.available = true;
                sensor.kind = hardwarescope::SensorKind::frame_rate;
                sensor.unit = hardwarescope::SensorUnit::frames_per_second;
                sensor.current = value;
                wcscpy_s(sensor.hardware.data(), sensor.hardware.size(), L"cs2.exe");
            };
            add(hardwarescope::kFpsSensorId, reading.frames_per_second);
            if (reading.one_percent_low_frames_per_second) add(hardwarescope::kFpsOnePercentLowSensorId, reading.one_percent_low_frames_per_second);
        }
        auto items = hardwarescope::BuildOsdSurfaceItems(*snapshot, settings, false);
        // 1% lows intentionally remain absent until 100 actual intervals exist.
        const auto has_low = reading.one_percent_low_frames_per_second != 0U;
        if (has_low) ++low_available;
        if (reading.available && (items.size() != (has_low ? 2U : 1U)
            || items[0].text != L"FPS " + std::to_wstring(reading.frames_per_second)
            || (has_low && items[1].text != L"1% Low " + std::to_wstring(reading.one_percent_low_frames_per_second)))) ++osd_mismatches;
        osd->Update(*snapshot);
        hardwarescope::ResetSnapshot(*installed);
        double installed_fps{};
        if (installed_bridge.CollectFrameRates(*installed)) {
            for (std::uint32_t index{}; index < installed->count; ++index) {
                if (installed->sensors[index].id == hardwarescope::kFpsSensorId && installed->sensors[index].available) {
                    installed_fps = installed->sensors[index].current;
                    ++bridge_available;
                }
            }
        }
        log << elapsed << ',' << reading.available << ',' << reading.frames_per_second << ','
            << reading.one_percent_low_frames_per_second << ',' << reading.frame_time_milliseconds << ','
            << items.size() << ',' << installed_fps << ',' << reading.frame_qpc << ',' << reading.low_frame_qpc << ','
            << reading.low_interval_count << ',' << qpc.QuadPart << '\n';
        log.flush();
        Sleep(100U);
    }
    runner->Stop();
    const auto stopped = runner->Snapshot();
    hardwarescope::ResetSnapshot(*snapshot);
    osd->Update(*snapshot);
    const auto cleared = hardwarescope::BuildOsdSurfaceItems(*snapshot, settings, false).empty();
    log << "summary,available_samples," << available << ",low_samples," << low_available << ",osd_mismatches," << osd_mismatches
        << ",installed_bridge_samples," << bridge_available << ",stopped_unavailable," << !stopped.available
        << ",empty_osd_cleared," << cleared << ",unavailable_after_start," << unavailable_after_start << '\n';
    // Intended for a continuously rendering game: raw-event analysis is still
    // required to distinguish game pauses from capture-delivery outages.
    const auto passed = available >= 100U && low_available >= 100U && osd_mismatches == 0U
        && unavailable_after_start == 0U && !stopped.available && cleared;
    log << (passed ? "PASS live capture/math/OSD component path\n" : "FAIL live component path\n");
    return passed ? 0 : 1;
}
