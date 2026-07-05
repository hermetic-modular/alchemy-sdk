# Host-only unit tests.  Included from the root CMakeLists when not
# cross-compiling; run with `ctest --preset host` or the test binary
# directly.

add_executable(ring_frame_test ring_frame_test.cpp)
target_link_libraries(ring_frame_test PRIVATE alchemy::primitives)

add_test(NAME ring_frame COMMAND ring_frame_test)
