# stereo_eq — full opt-in surface stack: ControlLoop + VirtualKnob +
# Pager + ParamLock + Presets.  DSP and palettes are split into separate
# translation units so stereo_eq.cpp stays a pure SDK example.

daisy_executable(
    NAME    stereo_eq
    SOURCES stereo_eq.cpp stereo_eq_dsp.cpp
    LIBS    alchemy::surface)
