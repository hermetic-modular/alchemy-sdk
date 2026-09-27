# v2_cal_test — scope-driven calibration acceptance test (V2 only).
#
# B1 steps all six jacks through −5..+5 V in 1 V increments; B2 toggles
# calibrated vs design-nominal output so the correction delta is visible
# on a scope. See the source header for the full bench procedure.

if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()
if(NOT ALCHEMY_BOARD STREQUAL "v2")
    return()
endif()

daisy_executable(
    NAME    v2_cal_test
    SOURCES v2_cal_test.cpp
    LIBS    alchemy::board_alchemy_lab_v2 alchemy::host_link)
