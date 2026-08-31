# Host-only unit tests.  Included from the root CMakeLists when not
# cross-compiling; run with `ctest --preset host` or the test binary
# directly.

add_executable(ring_frame_test ring_frame_test.cpp)
target_link_libraries(ring_frame_test PRIVATE alchemy::primitives)

add_test(NAME ring_frame COMMAND ring_frame_test)

add_executable(clip_indicator_test clip_indicator_test.cpp)
target_link_libraries(clip_indicator_test PRIVATE alchemy::primitives)

add_test(NAME clip_indicator COMMAND clip_indicator_test)

# CvInput is fully inline, so its transfer math compiles on host from the
# V2 headers alone — no V2 BSP target needed (that one is firmware-only).
# Stubs (daisy_seed.h) arrive through alchemy::primitives' include paths.
add_executable(cv_input_test cv_input_test.cpp)
target_include_directories(cv_input_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../hardware/alchemy-lab/v2/include")
target_link_libraries(cv_input_test PRIVATE alchemy::primitives)

add_test(NAME cv_input COMMAND cv_input_test)

add_executable(clock_follower_test clock_follower_test.cpp)
target_link_libraries(clock_follower_test PRIVATE alchemy::primitives)

add_test(NAME clock_follower COMMAND clock_follower_test)

add_executable(pot_gate_test pot_gate_test.cpp)
target_link_libraries(pot_gate_test PRIVATE alchemy::primitives)

add_test(NAME pot_gate COMMAND pot_gate_test)

add_subdirectory(host)
