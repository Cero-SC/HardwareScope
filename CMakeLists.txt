cmake_minimum_required(VERSION 3.28)

project(HardwareScopeNative VERSION 2.0.13 LANGUAGES CXX RC)

# Refuse to package an unexpected prerequisite, even in offline/local builds.
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pawnio/PawnIO_setup.exe" PAWNIO_SHA256)
if(NOT PAWNIO_SHA256 STREQUAL "1f519a22e47187f70a1379a48ca604981c4fcf694f4e65b734aaa74a9fba3032")
    message(FATAL_ERROR "Bundled PawnIO 2.2.0 checksum mismatch")
endif()

configure_file(installer/HardwareScope.iss.in HardwareScope.iss @ONLY)
configure_file(resources/HardwareScopeVersion.rc.in HardwareScopeVersion.rc @ONLY)
set(HARDWARESCOPE_VERSION_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/HardwareScopeVersion.rc")

option(HARDWARESCOPE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

if(MSVC)
    # Keep the public installer and portable ZIP self-contained. A clean Windows
    # machine must not need a separately installed Visual C++ redistributable.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(hardwarescope_core STATIC
    src/amd_gpu_provider.cpp
    src/amd_zen_provider.cpp
    src/app_settings.cpp
    src/ddr5_temperature_provider.cpp
    src/file_verification.cpp
    src/frame_rate_tracker.cpp
    src/game_detector.cpp
    src/graph_model.cpp
    src/legacy_settings_migration.cpp
    src/monitoring_control.cpp
    src/osd_model.cpp
    src/nvidia_gpu_provider.cpp
    src/nct6687_provider.cpp
    src/pawn_io.cpp
    src/presentmon_fps_runner.cpp
    src/privileged_sensor_collector.cpp
    src/sensor_bridge.cpp
    src/sensor_explanations.cpp
    src/sensor_worker.cpp
    src/sensor_view_model.cpp
    src/storage_temperature_provider.cpp
    src/startup_registration.cpp
    src/system_performance_provider.cpp
    src/ui_palette.cpp
    src/update_manifest.cpp
    src/update_client.cpp
    src/update_coordinator.cpp
)

target_include_directories(hardwarescope_core PUBLIC include)
target_compile_features(hardwarescope_core PUBLIC cxx_std_23)
target_compile_definitions(hardwarescope_core PUBLIC
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    HARDWARESCOPE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    HARDWARESCOPE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    HARDWARESCOPE_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)
target_link_libraries(hardwarescope_core PUBLIC shell32 ole32 powrprof advapi32 bcrypt comctl32 dwmapi uxtheme winhttp)

set(HARDWARESCOPE_APP_SOURCES
    src/main.cpp
    src/graph_window.cpp
    src/native_window.cpp
    src/osd_window.cpp
    src/settings_window.cpp
    src/tray_panel_window.cpp
    src/update_prompt_window.cpp
    resources/HardwareScope.rc
    ${HARDWARESCOPE_VERSION_RESOURCE}
)

function(add_hardwarescope_app target_name internal_test_hooks)
    add_executable(${target_name} WIN32 ${HARDWARESCOPE_APP_SOURCES})
    # Merge our DPI/UAC/Common-Controls declarations into the linker-generated
    # embedded manifest. An RT_MANIFEST entry in the .rc file is overwritten by
    # MSVC's default manifest step and therefore cannot carry these declarations.
    target_link_options(${target_name} PRIVATE
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/resources/app.manifest"
    )
    target_link_libraries(${target_name} PRIVATE
        hardwarescope_core
        d2d1
        dwrite
        dwmapi
        shcore
        comctl32
        ole32
        gdi32
        comdlg32
        windowscodecs
    )
    target_compile_definitions(${target_name} PRIVATE
        HARDWARESCOPE_VERSION=L"${PROJECT_VERSION}"
        HARDWARESCOPE_INTERNAL_TEST_HOOKS=${internal_test_hooks}
    )
endfunction()

# The packaged app contains no test-only window messages. Automated UI and soak
# probes run against the separate instrumented binary built from the same code.
add_hardwarescope_app(HardwareScopeNative 0)
add_hardwarescope_app(HardwareScopeNativeInstrumented 1)

add_executable(HardwareScopeNativeTests
    tests/core_tests.cpp
)
target_link_libraries(HardwareScopeNativeTests PRIVATE hardwarescope_core)

add_executable(HardwareScopeNativeUiSmokeTests
    tests/ui_smoke_tests.cpp
)
target_include_directories(HardwareScopeNativeUiSmokeTests PRIVATE include)
target_compile_features(HardwareScopeNativeUiSmokeTests PRIVATE cxx_std_23)
target_link_libraries(HardwareScopeNativeUiSmokeTests PRIVATE user32)

add_executable(HardwareScopeNativeUpdateUiSmokeTests
    tests/update_ui_smoke_tests.cpp
)
target_include_directories(HardwareScopeNativeUpdateUiSmokeTests PRIVATE include)
target_link_libraries(HardwareScopeNativeUpdateUiSmokeTests PRIVATE user32)

add_executable(HardwareScopeNativeHardwareProbe
    tests/hardware_probe.cpp
    resources/HardwareProbe.rc
)
target_link_libraries(HardwareScopeNativeHardwareProbe PRIVATE hardwarescope_core)

add_executable(HardwareScopeNativeSystemProbe
    tests/system_probe.cpp
)
target_link_libraries(HardwareScopeNativeSystemProbe PRIVATE hardwarescope_core)

add_executable(HardwareScopeNativeGpuProbe
    tests/gpu_probe.cpp
)
target_link_libraries(HardwareScopeNativeGpuProbe PRIVATE hardwarescope_core)

add_executable(HardwareScopeNativeSensorService
    src/sensor_service_main.cpp
    resources/SensorService.rc
    ${HARDWARESCOPE_VERSION_RESOURCE}
)
target_link_libraries(HardwareScopeNativeSensorService PRIVATE hardwarescope_core advapi32)
add_custom_command(TARGET HardwareScopeNativeSensorService POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/presentmon/PresentMon-2.4.1-x64.exe"
        "$<TARGET_FILE_DIR:HardwareScopeNativeSensorService>/PresentMon.exe")

add_executable(HardwareScopeNativeUpdater WIN32
    src/updater_main.cpp
    resources/Updater.rc
    ${HARDWARESCOPE_VERSION_RESOURCE}
)
target_link_libraries(HardwareScopeNativeUpdater PRIVATE hardwarescope_core shell32)

add_executable(HardwareScopeNativeUpdateRelaunchFixture WIN32
    tests/update_relaunch_fixture.cpp
)
target_compile_features(HardwareScopeNativeUpdateRelaunchFixture PRIVATE cxx_std_23)
target_compile_definitions(HardwareScopeNativeUpdateRelaunchFixture PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(HardwareScopeNativeUpdaterRecoveryTests
    tests/updater_recovery_tests.cpp
)
target_compile_features(HardwareScopeNativeUpdaterRecoveryTests PRIVATE cxx_std_23)
target_compile_definitions(HardwareScopeNativeUpdaterRecoveryTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(HardwareScopeNativeUpdaterRecoveryTests HardwareScopeNativeUpdater HardwareScopeNativeUpdateRelaunchFixture)

add_executable(HardwareScopeNativeUpdateProbe
    tests/update_probe.cpp
)
target_link_libraries(HardwareScopeNativeUpdateProbe PRIVATE hardwarescope_core)

add_executable(HardwareScopeNativeBridgeProbe
    tests/bridge_probe.cpp
)
target_link_libraries(HardwareScopeNativeBridgeProbe PRIVATE hardwarescope_core)

add_executable(HardwareScopeNativeFpsProbe
    tests/fps_probe.cpp
    src/etw_fps_monitor.cpp
)
target_link_libraries(HardwareScopeNativeFpsProbe PRIVATE hardwarescope_core)

add_executable(HardwareScopeNativePresentMonRunnerProbe
    tests/presentmon_runner_probe.cpp
)
target_link_libraries(HardwareScopeNativePresentMonRunnerProbe PRIVATE hardwarescope_core)
add_custom_command(TARGET HardwareScopeNativePresentMonRunnerProbe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/presentmon/PresentMon-2.4.1-x64.exe"
        "$<TARGET_FILE_DIR:HardwareScopeNativePresentMonRunnerProbe>/PresentMon.exe")

add_executable(HardwareScopeNativeFpsGameFixture-Win64-Shipping WIN32
    tests/fps_game_fixture.cpp
)
target_link_libraries(HardwareScopeNativeFpsGameFixture-Win64-Shipping PRIVATE d3d11 dxgi user32)

add_executable(HardwareScopeNativeWorkerProbe
    tests/native_worker_probe.cpp
)
target_link_libraries(HardwareScopeNativeWorkerProbe PRIVATE hardwarescope_core)

add_executable(HardwareScopeNativeSoakProbe
    tests/soak_probe.cpp
)
target_include_directories(HardwareScopeNativeSoakProbe PRIVATE include)
target_compile_features(HardwareScopeNativeSoakProbe PRIVATE cxx_std_23)
target_compile_definitions(HardwareScopeNativeSoakProbe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(HardwareScopeNativeSoakProbe PRIVATE user32 psapi)

add_executable(HardwareScopeNativeSoakStressProbe
    tests/soak_probe.cpp
)
target_include_directories(HardwareScopeNativeSoakStressProbe PRIVATE include)
target_compile_features(HardwareScopeNativeSoakStressProbe PRIVATE cxx_std_23)
target_compile_definitions(HardwareScopeNativeSoakStressProbe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX HARDWARESCOPE_SOAK_DEFAULT_STRESS=1)
target_link_libraries(HardwareScopeNativeSoakStressProbe PRIVATE user32 psapi)

add_executable(HardwareScopeNativeSensorExport
    tests/sensor_export.cpp
    resources/SensorExport.rc
)
target_link_libraries(HardwareScopeNativeSensorExport PRIVATE hardwarescope_core)

foreach(target hardwarescope_core HardwareScopeNative HardwareScopeNativeInstrumented HardwareScopeNativeTests HardwareScopeNativeUiSmokeTests HardwareScopeNativeUpdateUiSmokeTests HardwareScopeNativeHardwareProbe HardwareScopeNativeSystemProbe HardwareScopeNativeGpuProbe HardwareScopeNativeSensorService HardwareScopeNativeUpdater HardwareScopeNativeUpdateRelaunchFixture HardwareScopeNativeUpdaterRecoveryTests HardwareScopeNativeUpdateProbe HardwareScopeNativeBridgeProbe HardwareScopeNativeFpsProbe HardwareScopeNativePresentMonRunnerProbe HardwareScopeNativeFpsGameFixture-Win64-Shipping HardwareScopeNativeWorkerProbe HardwareScopeNativeSoakProbe HardwareScopeNativeSoakStressProbe HardwareScopeNativeSensorExport)
    target_compile_options(${target} PRIVATE
        /W4
        /permissive-
        /Zc:__cplusplus
        /Zc:preprocessor
        /utf-8
        /EHsc
        $<$<BOOL:${HARDWARESCOPE_WARNINGS_AS_ERRORS}>:/WX>
    )
endforeach()

include(CTest)
if(BUILD_TESTING)
    add_test(NAME HardwareScopeNative.Core COMMAND HardwareScopeNativeTests)
    add_test(NAME HardwareScopeNative.UpdaterRecovery COMMAND HardwareScopeNativeUpdaterRecoveryTests)
    add_test(NAME HardwareScopeNative.ReleaseEvidence
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_evidence_tests.ps1")
endif()

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT HardwareScopeNative)

install(PROGRAMS $<TARGET_FILE:HardwareScopeNative> DESTINATION . RENAME HardwareScope.exe)
install(PROGRAMS $<TARGET_FILE:HardwareScopeNativeUpdater> DESTINATION . RENAME HardwareScopeUpdater.exe)
install(PROGRAMS $<TARGET_FILE:HardwareScopeNativeSensorService> DESTINATION . RENAME HardwareScopeSensorService.exe)
install(PROGRAMS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/presentmon/PresentMon-2.4.1-x64.exe" DESTINATION . RENAME PresentMon.exe)
install(FILES LICENSE.txt THIRD_PARTY_NOTICES.md PRIVACY.md CODE_SIGNING_POLICY.md README.md DESTINATION .)
install(PROGRAMS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pawnio/PawnIO_setup.exe" DESTINATION prerequisites)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pawnio/README.md" DESTINATION prerequisites)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/PawnIO-Modules-COPYING.txt" DESTINATION .)

set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME HardwareScope)
set(CPACK_PACKAGE_VENDOR HardwareScope)
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME "HardwareScope-${PROJECT_VERSION}-x64")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
include(CPack)
