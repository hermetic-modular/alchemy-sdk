# ring_demo — ring composition demo.
#
# A stereo tremolo whose rings render the LFO itself: a comet pip
# overdrawn on the rate ring, and a fully custom depth ring with a
# carved notch and a shimmer field.

daisy_executable(
    NAME    ring_demo
    SOURCES ring_demo.cpp
    LIBS    alchemy::surface)
