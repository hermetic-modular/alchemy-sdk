cmake_minimum_required(VERSION 3.21)

project(AlchemySDK LANGUAGES CXX C ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ── Cross-compile branch: bring in libDaisy and the daisy_executable helper
if(CMAKE_CROSSCOMPILING)
    message(STATUS "alchemy-sdk: cross-compile build (target: Daisy Seed2 DFM, QSPI bootloader)")

    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/vendor/libDaisy/CMakeLists.txt")
        message(FATAL_ERROR
            "alchemy-sdk: vendor/libDaisy is not initialised.  Run:\n"
            "    git submodule update --init --recursive\n"
            "from ${CMAKE_SOURCE_DIR}")
    endif()

    add_subdirectory(vendor/libDaisy EXCLUDE_FROM_ALL)

    # libDaisy's stock Makefile sets -DBOOT_APP when building the startup
    # file so Reset_Handler skips SystemInit() (the Daisy bootloader has
    # already configured the clock tree).  Its CMakeLists.txt doesn't do
    # this, so we have to set it ourselves — PUBLIC so it applies both to
    # libdaisy's own translation units (notably startup_stm32h750xx.c)
    # and to every consumer of the `daisy` target.  Without this, the
    # Reset_Handler re-inits clocks already running and hard-faults
    # immediately on boot.
    # TODO research if there is a more Daisy idiomatic way.
    target_compile_definitions(daisy PUBLIC BOOT_APP)

    include(AlchemySdk)

# ── Host branch: inject the libDaisy stubs so headers resolve ──────────────
else()
    set(ALCHEMY_STUB_DIR "${CMAKE_SOURCE_DIR}/stubs")
    message(STATUS "alchemy-sdk: host build — using libDaisy stubs from ${ALCHEMY_STUB_DIR}")
endif()

add_compile_options(
    -Wall
    -Wextra
    -Wshadow
    -Wno-unused-parameter    # libDaisy stubs have intentional unused params
    $<$<CXX_COMPILER_ID:GNU>:-Wno-maybe-uninitialized>
)

# ── Library layers ─────────────────────────────────────────────────────────
# v2 is the board.  Host builds are board-free (primitives + tests only);
# the legacy v1 BSP builds only when -DALCHEMY_BOARD=v1 is passed
# explicitly, with a warning.
set(ALCHEMY_BOARD "v2" CACHE STRING "Alchemy Lab board version (v1 | v2)")

if(CMAKE_CROSSCOMPILING)
    if(ALCHEMY_BOARD STREQUAL "v1")
        message(WARNING
            "alchemy-sdk: building for the LEGACY v1 board. A v1 binary "
            "on v2 hardware drives wrong LED and control mappings.")
        add_subdirectory(hardware/alchemy-lab/v1)
    else()
        add_subdirectory(hardware/alchemy-lab/v2)
    endif()
endif()
add_subdirectory(framework)

# ── Tests (host only) ──────────────────────────────────────────────────────
if(NOT CMAKE_CROSSCOMPILING)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Examples (firmware — cross-compile only) ──────────────────────────────
if(CMAKE_CROSSCOMPILING)
    add_subdirectory(examples/stereo_eq)
    add_subdirectory(examples/kick)
    add_subdirectory(examples/clock_sync)
    add_subdirectory(examples/ring_demo)
    add_subdirectory(examples/v2_cv_demo)
    add_subdirectory(examples/v2_cal_test)
    add_subdirectory(examples/cv_playground)
endif()
