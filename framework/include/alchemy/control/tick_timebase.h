/**
 * @file tick_timebase.h
 * @brief Convert a wrapping hardware tick counter to wrapping microseconds.
 */
#pragma once

#include <cassert>
#include <cstdint>

namespace alchemy {

/**
 * Converts raw uint32_t ticks to microseconds wrapping at 2^32 us.
 * Initialize after hardware startup; use from one polling context only.
 * Update gaps must be less than 2^32 / frequency_hz seconds (17.9 s at
 * 240 MHz). Reset this instance and its consumers if the timer restarts
 * or changes frequency. Consumers must share the same epoch.
 */
class TickTimebase
{
  public:
    /** Establish an epoch; initial_us is normally zero. frequency_hz > 0. */
    void Init(uint32_t frequency_hz, uint32_t raw_tick,
              uint32_t initial_us = 0u)
    {
        assert(frequency_hz > 0u);
        frequency_hz_ = frequency_hz;
        last_tick_ = raw_tick;
        now_us_ = initial_us;
        remainder_ = 0u;
    }

    bool Initialized() const { return frequency_hz_ != 0u; }

    /** Advance from a new raw sample. Init must have been called first. */
    uint32_t Update(uint32_t raw_tick)
    {
        assert(Initialized());
        const uint32_t delta = raw_tick - last_tick_;
        last_tick_ = raw_tick;

        // Preserve fractional microseconds between updates.
        const uint64_t scaled = static_cast<uint64_t>(delta) * 1000000u
                              + remainder_;
        now_us_ += static_cast<uint32_t>(scaled / frequency_hz_);
        remainder_ = static_cast<uint32_t>(scaled % frequency_hz_);
        return now_us_;
    }

    uint32_t NowUs() const { return now_us_; }

  private:
    uint32_t frequency_hz_ = 0u;
    uint32_t last_tick_ = 0u;
    uint32_t now_us_ = 0u;
    uint32_t remainder_ = 0u;
};

} // namespace alchemy
