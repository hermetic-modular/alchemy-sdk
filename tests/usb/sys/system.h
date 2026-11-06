#pragma once
#include "daisy_seed.h"
#include "util/scopedirqblocker.h"
inline uint32_t& test_tick = daisy::System::TickForUnitTest();
inline uint32_t test_isr = 0;
inline uint32_t __get_IPSR() { return test_isr; }
inline uint32_t __get_PRIMASK() { return test_primask; }
inline uint32_t __get_BASEPRI() { return 0; }
