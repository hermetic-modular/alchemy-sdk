#include "alchemy/control/tick_timebase.h"
#include "alchemy/control/musical_clock.h"
#include "alchemy/control/clock_follower.h"
#include "alchemy/control/cv_edge.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++failures; } } while (0)

constexpr uint32_t kMax = std::numeric_limits<uint32_t>::max();

void TestConversion(uint32_t frequency)
{
    alchemy::TickTimebase time;
    CHECK(!time.Initialized());
    const uint32_t origin = kMax - 17u;
    time.Init(frequency, origin, kMax - 100u);
    CHECK(time.Initialized());
    CHECK(time.RawTick() == origin);
    CHECK(time.Update(origin) == kMax - 100u);
    uint64_t total_ticks = 0u;
    uint32_t random = 123u;
    for (int i = 0; i < 10000; ++i)
    {
        random = random * 1664525u + 1013904223u;
        // Alternate fractional-us samples and gaps below one raw wrap.
        const uint32_t delta = i % 2 ? random : random % 100u;
        total_ticks += delta;
        const uint32_t expected = (kMax - 100u) + static_cast<uint32_t>(
            (total_ticks / frequency) * 1000000u
            + (total_ticks % frequency) * 1000000u / frequency);
        CHECK(time.Update(origin + static_cast<uint32_t>(total_ticks)) == expected);
        CHECK(time.RawTick() == origin + static_cast<uint32_t>(total_ticks));
    }
    // Reset elapsed time and fractional remainder.
    time.Init(frequency, 123u);
    CHECK(time.NowUs() == 0u);
    CHECK(time.Update(123u + frequency) == 1000000u);
    // Largest interval below one raw wrap.
    time.Init(frequency, 123u);
    CHECK(time.Update(122u) == static_cast<uint32_t>(
        static_cast<uint64_t>(kMax) * 1000000u / frequency));
}

void TestFractionalSamples()
{
    alchemy::TickTimebase time;
    time.Init(240000000u, 0u);
    for (uint32_t tick = 1u; tick <= 240000u; ++tick)
        CHECK(time.Update(tick) == tick / 240u);
    CHECK(time.NowUs() == 1000u);
}

void TestObservedTimestamps(uint32_t frequency)
{
    alchemy::TickTimebase time;
    const uint32_t origin = kMax - 100u;
    const uint32_t initial_us = kMax - 10u;
    time.Init(frequency, origin, initial_us);
    uint32_t stamp = 0u;
    CHECK(!alchemy::TickTimebase::Sample{}.TimeOf(origin, stamp));
    for (uint32_t tick = 1u; tick < 1200u; ++tick) {
        time.Update(origin + tick);
        const auto sample = time.GetSample();
        CHECK(sample.Valid() && sample.raw_tick == time.RawTick() && sample.time_us == time.NowUs());
        for (uint32_t observed = 0u; observed <= tick; observed += 11u) {
            CHECK(sample.TimeOf(origin + observed, stamp));
            CHECK(stamp == initial_us + static_cast<uint32_t>(uint64_t(observed) * 1000000u / frequency));
        }
        CHECK(!sample.TimeOf(origin + tick + 1u, stamp));
    }
    time.Update(origin + frequency * 3u);
    CHECK(!time.GetSample().TimeOf(origin, stamp));
    CHECK(time.GetSample().TimeOf(origin, stamp, 3000000u) && stamp == initial_us);
}

void TestMusicalRollover(uint32_t frequency)
{
    alchemy::TickTimebase time;
    alchemy::MusicalClock clock;
    const uint32_t interval_ticks = frequency / 1000u;
    const uint32_t origin = kMax - interval_ticks / 2u;
    time.Init(frequency, origin);
    clock.SetBpm(120.f);
    clock.Start();
    clock.Tick(time.NowUs());
    clock.Tick(time.Update(origin + interval_ticks));
    const double integrated_us = (clock.MasterTick() + clock.FracTick())
                               / clock.TicksPerUs();
    CHECK(std::fabs(integrated_us - 1000.0) < 1e-6);
    // Preserve the intentional 50 ms clamp for real stalls.
    clock.Tick(time.Update(origin + interval_ticks + frequency / 10u));
    const double after_stall = (clock.MasterTick() + clock.FracTick())
                            / clock.TicksPerUs();
    CHECK(std::fabs(after_stall - 51000.0) < 1e-6);
}

struct ClockRig
{
    alchemy::MusicalClock clock;
    alchemy::ClockFollower follower{clock};
    alchemy::CvEdge edge;

    ClockRig()
    {
        edge.Init(1u);
        follower.SetAutoStart(true);
        follower.Enable(24u);
    }

    void Poll(float cv, uint32_t now)
    {
        edge.Tick(&cv, 1u, now);
        if (edge.JustRose(0u)) follower.OnPulse(edge.LastRiseUs(0u));
        follower.Update(now);
        clock.Tick(now);
    }
};

void TestFollowerAndEdges(uint32_t frequency, uint32_t initial_us)
{
    alchemy::TickTimebase time;
    ClockRig actual, reference;
    const uint32_t origin = kMax - frequency / 2u;
    time.Init(frequency, origin, initial_us);
    // Compare 125 BPM / 24 PPQN against wall time across raw and us wraps.
    for (uint32_t ms = 0u; ms < 65000u; ++ms)
    {
        const uint32_t elapsed_us = ms * 1000u;
        const uint32_t raw = origin + static_cast<uint32_t>(
            static_cast<uint64_t>(ms) * frequency / 1000u);
        const float cv = ms % 20u < 2u ? 1.f : 0.f;
        actual.Poll(cv, time.Update(raw));
        reference.Poll(cv, initial_us + elapsed_us);
        CHECK(actual.clock.MasterTick() == reference.clock.MasterTick());
        CHECK(std::fabs(actual.clock.FracTick() - reference.clock.FracTick()) < 1e-9);
        CHECK(actual.clock.Events() == reference.clock.Events());
        if (ms > 1000u) CHECK(actual.follower.Locked());
    }
    CHECK(std::fabs(actual.follower.Bpm() - 125.f) < 0.01f);
}

void TestDebounce(uint32_t frequency)
{
    alchemy::TickTimebase time;
    alchemy::CvEdge edge;
    alchemy::CvEdge::Config cfg;
    cfg.debounce_us = 5000u;
    edge.Init(1u, cfg);
    const uint32_t origin = kMax - frequency / 1000u;
    time.Init(frequency, origin, kMax - 1000u);
    float cv = 1.f;
    CHECK(edge.Tick(&cv, 1u, time.NowUs()) == 1u);
    cv = 0.f;
    edge.Tick(&cv, 1u, time.Update(origin + frequency / 2000u));
    cv = 1.f;
    // Both raw ticks and microseconds have wrapped; this edge is too soon.
    CHECK(edge.Tick(&cv, 1u, time.Update(origin + frequency / 500u)) == 0u);
    CHECK(edge.Tick(&cv, 1u, time.Update(origin + frequency / 200u)) == 1u);
}
} // namespace

int main()
{
    const uint32_t frequencies[] = {200000000u, 240000000u};
    for (const uint32_t frequency : frequencies)
    {
        TestConversion(frequency);
        TestObservedTimestamps(frequency);
        TestMusicalRollover(frequency);
        TestFollowerAndEdges(frequency, 0u);
        TestFollowerAndEdges(frequency, kMax - 100000u);
        TestDebounce(frequency);
    }
    TestConversion(123456789u); // conversion does not assume integer MHz
    TestObservedTimestamps(123456789u);
    TestFractionalSamples();
    if (failures) return 1;
    std::puts("all tick timebase tests passed");
    return 0;
}
