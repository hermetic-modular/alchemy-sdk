# hostlink — browser preset & settings management over USB CDC.
#
# Cross-compile only (the CDC transport drives real USB hardware) and V2
# only (the panel USB-C rides the Seed2 DFM's EXTERNAL USB pins).

if(NOT CMAKE_CROSSCOMPILING)
    return()
endif()
if(NOT ALCHEMY_BOARD STREQUAL "v2")
    return()
endif()

daisy_executable(
    NAME    hostlink
    SOURCES hostlink.cpp
    LIBS    alchemy::surface alchemy::host_link)
