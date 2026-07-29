# HostLink protocol + descriptor host tests.
#
#   cmake --preset host && cmake --build --preset host
#   ctest --test-dir build-host --output-on-failure
#
# The test binary doubles as the golden-vector generator for cross-language
# interop tests (the website's TypeScript codec):
#
#   build-host/tests/host/hostlink_tests --emit-golden tests/host/golden/hostlink_golden.json
#
# Host builds are board-free (no alchemy::surface / BSP targets), so the
# binary compiles the framework and V1-BSP translation units it exercises
# directly — stub-backed through alchemy::primitives' include paths.

set(SDK_ROOT "${CMAKE_SOURCE_DIR}")

add_executable(hostlink_tests
    hostlink_tests.cpp
    button_tests.cpp
    fs_tests.cpp
    param_lock_tests.cpp
    ram_diskio.cpp
    "${SDK_ROOT}/framework/src/host_link/host_link.cpp"
    "${SDK_ROOT}/framework/src/host_link/descriptor.cpp"
    "${SDK_ROOT}/framework/src/host_link/describe.cpp"
    "${SDK_ROOT}/framework/src/host_link/json_check.cpp"
    "${SDK_ROOT}/framework/src/storage/sd_card.cpp"
    "${SDK_ROOT}/framework/src/storage/fs_extension.cpp"
    "${SDK_ROOT}/framework/src/surface/button_bank.cpp"
    "${SDK_ROOT}/framework/src/surface/pager.cpp"
    "${SDK_ROOT}/framework/src/surface/settings.cpp"
    "${SDK_ROOT}/framework/src/surface/presets.cpp"
    "${SDK_ROOT}/framework/src/surface/preset_gesture_ui.cpp"
    "${SDK_ROOT}/framework/src/surface/virtual_knob.cpp"
    "${SDK_ROOT}/framework/src/control/param_lock_manager.cpp"
    "${SDK_ROOT}/hardware/alchemy-lab/v1/src/ws2812.cpp"
    "${SDK_ROOT}/hardware/alchemy-lab/v1/src/alchemy_lab_v1.cpp"
    "${SDK_ROOT}/hardware/alchemy-lab/v1/src/alchemy_lab_v1_layout.cpp"
    # The real FatFS, backed by the RAM diskio (ram_diskio.cpp) — the
    # storage layer is tested against the production filesystem code.
    "${SDK_ROOT}/vendor/libDaisy/Middlewares/Third_Party/FatFs/src/ff.c"
    "${SDK_ROOT}/vendor/libDaisy/Middlewares/Third_Party/FatFs/src/option/ccsbcs.c"
)
target_include_directories(hostlink_tests PRIVATE
    "${SDK_ROOT}/hardware/alchemy-lab/v1/include"
    "${SDK_ROOT}/hardware/alchemy-lab/v1/src"
    # FatFS headers + the target's exact ffconf.h (via the shim dir,
    # which avoids exposing the rest of libDaisy src/sys).
    "${CMAKE_CURRENT_SOURCE_DIR}/fatfs_conf"
    "${SDK_ROOT}/vendor/libDaisy/Middlewares/Third_Party/FatFs/src"
)
# Test at the PRODUCTION frame size (storage modules ship 2048).  This is
# load-bearing: FatFS only takes its direct-DMA path when a transfer
# spans whole sectors, which 512-byte bodies never do — the REC0001.WAV
# download bug (unaligned direct-DMA destinations) is invisible at 512.
target_compile_definitions(hostlink_tests PRIVATE ALCHEMY_HOSTLINK_MAX_BODY=2048)
target_link_libraries(hostlink_tests PRIVATE alchemy::primitives)

add_test(NAME hostlink COMMAND hostlink_tests)
