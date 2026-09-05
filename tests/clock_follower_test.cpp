/**
 * @file clock_follower_test.cpp
 * @brief Host tests for ClockFollower under fast external clocks and
 *        delayed polls: pulse ring, k-pulse PI target, stamp-time phase.
 */

#include "alchemy/control/clock_follower.h"
#include "alchemy/control/musical_clock.h"

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                    \
        if (!(cond))                                                        \
        {                                                                   \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

constexpr double kExtPpqn = 24.0;

/* Deterministic jitter source. */
uint32_t Lcg(uint32_t& s)
{
    s = s * 1664525u + 1013904223u;
    return s;
}

/* External source + poll-loop rig.  Pulses are stamped at their exact
 * time (as an ISR would) even when the next poll runs late. */
struct Rig
{
    alchemy::MusicalClock  clock;
    alchemy::ClockFollower follower{clock};

    uint32_t now_us     = 0u;
    double   next_pulse = 1000.0;
    double   period_us  = 0.0;
    uint32_t events     = 0u;
    bool     tick_first = false;

    explicit Rig(double bpm)
    {
        SetSourceBpm(bpm);
        follower.SetAutoStart(true);
        follower.Enable(24u);
    }

    void SetSourceBpm(double bpm) { period_us = 60e6 / (bpm * kExtPpqn); }

    void Poll()
    {
        while (next_pulse <= static_cast<double>(now_us))
        {
            follower.OnPulse(static_cast<uint32_t>(next_pulse));
            next_pulse += period_us;
        }
        if (tick_first)
        {
            clock.Tick(now_us);
            follower.Update(now_us);
        }
        else
        {
            follower.Update(now_us);
            clock.Tick(now_us);
        }
        events |= clock.Events();
    }
};

/* 135 BPM / 24 PPQN (~18.5 ms pulses) polled at 1 ms with a 30–45 ms
 * frame stall every 25 polls, so drains carry 2–3 pulses.  The old
 * single-slot mailbox dropped all but the newest and wobbled by tens of
 * BPM; drain-time phase measurement hunted at ±3%. */
void TestFastClockSlowPolls()
{
    Rig      r(135.0);
    uint32_t seed    = 1u;
    double   max_dev = 0.0;
    int      poll_i  = 0;

    while (r.now_us < 8000000u)
    {
        r.Poll();
        if (r.now_us > 4000000u)
        {
            const double dev = std::fabs(static_cast<double>(r.clock.Bpm()) - 135.0);
            if (dev > max_dev) max_dev = dev;
        }
        r.now_us += (++poll_i % 25 == 0) ? 30000u + Lcg(seed) % 15001u : 1000u;
    }

    CHECK(r.follower.Locked());
    CHECK(std::fabs(r.follower.Bpm() - 135.0f) <= 0.5f);
    CHECK(max_dev <= 0.5);
}

/* Same schedule under both Update/Tick orders: the stamp-time anchor
 * (MusicalClock::LastTickUs) makes the wiring order non-critical. */
void TestOrderInsensitive()
{
    Rig a(135.0);
    Rig b(135.0);
    b.tick_first = true;

    uint32_t seed   = 7u;
    int      poll_i = 0;
    while (a.now_us < 6000000u)
    {
        a.Poll();
        b.Poll();
        const uint32_t step =
            (++poll_i % 25 == 0) ? 30000u + Lcg(seed) % 15001u : 1000u;
        a.now_us += step;
        b.now_us += step;
    }

    CHECK(std::fabs(a.clock.Bpm() - 135.0f) <= 0.5f);
    CHECK(std::fabs(b.clock.Bpm() - 135.0f) <= 0.5f);
    CHECK(std::fabs(a.clock.Bpm() - b.clock.Bpm()) <= 0.2f);
}

/* A 230 ms stall queues 12 pulses: the ring keeps 8 and drops 4, the
 * loss threshold trips, and the follower re-locks warm and converges. */
void TestRingOverflowRelock()
{
    Rig r(135.0);
    while (r.now_us < 2000000u) { r.Poll(); r.now_us += 1000u; }
    CHECK(r.follower.Locked());
    r.events = 0u;

    r.now_us += 230000u; /* stall: pulses queue, no polls */
    while (r.now_us < 6000000u) { r.Poll(); r.now_us += 1000u; }

    CHECK((r.events & alchemy::CLK_EV::EXT_LOST) != 0u);
    CHECK((r.events & alchemy::CLK_EV::EXT_LOCK) != 0u);
    CHECK(r.follower.Locked());
    CHECK(std::fabs(r.follower.Bpm() - 135.0f) <= 0.5f);
    CHECK(std::fabs(r.clock.Bpm() - 135.0f) <= 0.5f);
}

/* Source steps 120 → 140 BPM; follower re-converges without unlocking. */
void TestTempoStep()
{
    Rig r(120.0);
    while (r.now_us < 3000000u) { r.Poll(); r.now_us += 1000u; }
    CHECK(std::fabs(r.clock.Bpm() - 120.0f) <= 0.5f);

    r.SetSourceBpm(140.0);
    while (r.now_us < 9000000u) { r.Poll(); r.now_us += 1000u; }

    CHECK(r.follower.Locked());
    CHECK((r.events & alchemy::CLK_EV::EXT_LOST) == 0u);
    CHECK(std::fabs(r.follower.Bpm() - 140.0f) <= 0.5f);
    CHECK(std::fabs(r.clock.Bpm() - 140.0f) <= 0.5f);
}

/* A sub-millisecond double-trigger is rejected by the validity window
 * and must not perturb the estimate. */
void TestGlitchRejected()
{
    Rig r(135.0);
    while (r.now_us < 3000000u) { r.Poll(); r.now_us += 1000u; }
    const float before = r.follower.Bpm();

    const uint32_t glitch =
        static_cast<uint32_t>(r.next_pulse - r.period_us) + 300u;
    r.follower.OnPulse(glitch);
    for (int i = 0; i < 2000; ++i) { r.Poll(); r.now_us += 1000u; }

    CHECK(r.follower.Locked());
    CHECK(std::fabs(r.follower.Bpm() - before) <= 0.1f);
    CHECK(std::fabs(r.follower.Bpm() - 135.0f) <= 0.5f);
}

} // namespace

int main()
{
    TestFastClockSlowPolls();
    TestOrderInsensitive();
    TestRingOverflowRelock();
    TestTempoStep();
    TestGlitchRejected();

    if (failures)
    {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all clock follower tests passed\n");
    return 0;
}
