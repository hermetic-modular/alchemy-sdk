# kick — trigger-driven kick drum.
#
# A synthesis (instrument) example using ControlLoop + VirtualKnob + Page +
# ParamLock, with the envelope-meter overdraw via .Overdraw(fn) and the
# 1 ms B2 trigger polling via .OnPoll(fn). DSP is in a separate file so
# kick.cpp stays a pure SDK example.

daisy_executable(
    NAME    kick
    SOURCES kick.cpp kick_dsp.cpp
    LIBS    alchemy::surface)
