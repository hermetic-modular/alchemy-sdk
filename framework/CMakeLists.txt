# ── Framework library targets ──────────────────────────────────────────────
#
# alchemy::primitives  LedPanel, animations, pot-catch, CvRouter, PresetStore,
#                      ParamLockManager, PerfRenderer.  Pure algorithms — no
#                      buttons, no main loop, usable in any control style.
# alchemy::surface     Opt-in user-facing surfaces: Pager, Settings, ParamLock,
#                      LedRender, Presets, PresetGestureUi.  Each is a single
#                      constructor call away from being in the binary; nothing
#                      is auto-instantiated.

set(FRAMEWORK_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/include")
# Self-locating so the SDK can be add_subdirectory()'d from a consuming
# project (where CMAKE_SOURCE_DIR is the consumer, not the SDK root).
set(HW_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../hardware/include")

# ── alchemy::primitives ────────────────────────────────────────────────────

add_library(alchemy_primitives STATIC
    # LED panel
    src/led/panel.cpp

    # LED animations
    src/led/anims/fill.cpp
    src/led/anims/selector.cpp
    src/led/anims/pip.cpp
    src/led/anims/color_morph_arc.cpp
    src/led/anims/catch_pip.cpp
    src/led/anims/action_gesture.cpp
    src/led/anims/slot_indicator.cpp
    src/led/anims/sparkle.cpp
    src/led/anims/beat_pip.cpp
    src/led/anims/field.cpp
    src/led/ring_frame.cpp
    src/led/perf_renderer.cpp

    # Control primitives
    src/control/cv_router.cpp
    src/control/clock_pll.cpp
    src/control/musical_clock.cpp
    src/control/clock_follower.cpp
    src/control/cv_edge.cpp
    src/control/param_lock_manager.cpp
)
add_library(alchemy::primitives ALIAS alchemy_primitives)

target_include_directories(alchemy_primitives PUBLIC
    "${FRAMEWORK_INCLUDE}"
    "${HW_INCLUDE}"
)

# primitives uses libDaisy types (uint16_t typedefs, daisy::System::Delay, etc.)
# transitively via panel.cpp and the LED animations.  Link the daisy target
# directly rather than going through a specific BSP — that way V1 firmware and
# V2 firmware can each link primitives without pulling in the other board's
# concrete drivers (which share class names like alchemy::Ws2812Strip and would
# otherwise collide at link time).
if(CMAKE_CROSSCOMPILING)
    target_link_libraries(alchemy_primitives PUBLIC daisy)
elseif(DEFINED ALCHEMY_STUB_DIR)
    target_include_directories(alchemy_primitives PUBLIC "${ALCHEMY_STUB_DIR}")
endif()

# ── alchemy::surface (firmware — cross-compile only) ──────────────────────
# Surface links a board BSP; host builds are board-free and stop at
# alchemy::primitives + tests.

if(CMAKE_CROSSCOMPILING)

add_library(alchemy_surface STATIC
    src/surface/button_bank.cpp
    src/surface/pager.cpp
    src/surface/settings.cpp
    src/surface/led_render.cpp
    src/surface/presets.cpp
    src/surface/preset_gesture_ui.cpp
    src/surface/virtual_knob.cpp
    src/surface/control_loop.cpp
    src/surface/cv_matrix.cpp
)
add_library(alchemy::surface ALIAS alchemy_surface)

target_include_directories(alchemy_surface PUBLIC
    "${FRAMEWORK_INCLUDE}"
    "${HW_INCLUDE}"
)

# Board selector — picks which Alchemy Lab BSP `alchemy_surface` links and
# propagates the ALCHEMY_BOARD_V2 define to consumers so the unified shim
# headers (alchemy_lab.h, alchemy_lab_layout.h) resolve to the right board.
set(ALCHEMY_BOARD "v2" CACHE STRING "Alchemy Lab board version (v1 | v2)")
set_property(CACHE ALCHEMY_BOARD PROPERTY STRINGS v1 v2)

if(NOT ALCHEMY_BOARD STREQUAL "v1" AND NOT ALCHEMY_BOARD STREQUAL "v2")
    message(FATAL_ERROR
        "alchemy-sdk: ALCHEMY_BOARD must be 'v1' or 'v2' (got '${ALCHEMY_BOARD}').")
endif()

target_link_libraries(alchemy_surface PUBLIC
    alchemy::primitives
    alchemy::board_alchemy_lab_${ALCHEMY_BOARD}
)

if(ALCHEMY_BOARD STREQUAL "v2")
    target_compile_definitions(alchemy_surface PUBLIC ALCHEMY_BOARD_V2)
endif()

endif()

# ── alchemy::host_link (firmware — cross-compile only) ────────────────────
# Links alchemy::surface, so it exists only where surface does; host
# builds are board-free and exercise the protocol sources directly.

if(CMAKE_CROSSCOMPILING)

add_library(alchemy_host_link STATIC
    src/host_link/host_link.cpp
    src/host_link/descriptor.cpp
    src/host_link/describe.cpp
    src/host_link/json_check.cpp
    src/host_link/cdc_transport.cpp
    src/host_link/host.cpp
)
add_library(alchemy::host_link ALIAS alchemy_host_link)

target_include_directories(alchemy_host_link PUBLIC
    "${FRAMEWORK_INCLUDE}"
    "${HW_INCLUDE}"
)
target_link_libraries(alchemy_host_link PUBLIC alchemy::surface)

endif()

# ── alchemy::storage (firmware — cross-compile only) ───────────────────────
# The SD volume service (SdCard) + the HostLink §8 filesystem block
# (FsExtension).  Firmware that records/plays from SD without HostLink
# links this alone; the FsExtension additionally needs alchemy::host_link.
# Host builds exercise both through tests/host on the RAM diskio.

if(CMAKE_CROSSCOMPILING)

add_library(alchemy_storage STATIC
    src/storage/sd_card.cpp
    src/storage/fs_extension.cpp
)
add_library(alchemy::storage ALIAS alchemy_storage)

target_include_directories(alchemy_storage PUBLIC
    "${FRAMEWORK_INCLUDE}"
    "${HW_INCLUDE}"
    # FatFS headers ship with libDaisy (ffconf.h under src/sys, the core
    # under Middlewares); the daisy target carries src/sys transitively.
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/libDaisy/Middlewares/Third_Party/FatFs/src"
)
target_link_libraries(alchemy_storage PUBLIC daisy)

endif()
