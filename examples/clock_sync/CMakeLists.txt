# clock_sync — minimal demo of MusicalClock + ClockFollower.
#
# Reads an external CV clock (24 PPQN) on CV-1, follows it with a PI-PLL,
# and amplitude-modulates the audio passthrough by the locked beat phase.

daisy_executable(
    NAME    clock_sync
    SOURCES clock_sync.cpp
    LIBS    alchemy::surface)
