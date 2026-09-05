/**
 * @file pot_gate_test.cpp
 * @brief Host tests for the pot motion gate (timed UpdateCatch).
 */

#include "alchemy/control/pot_catch.h"

#include <cstdint>
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

/* Deterministic xorshift32 → uniform in [-1, 1). */
struct Rng
{
    uint32_t s = 0x1234567u;
    float    Next()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<float>(s) / 2147483648.0f - 1.0f;
    }
};

constexpr uint32_t kFrameMs = 16u;

/* Asleep + sub-threshold noise: stored stays bit-exact for 10 s. */
void TestHoldAsleep()
{
    alchemy::PotState s;
    Rng               rng;
    uint32_t          t = 0;
    for (int i = 0; i < 625; i++)
    {
        alchemy::UpdateCatch(s, 0.5f + 3e-4f * rng.Next(), t += kFrameMs);
        CHECK(s.stored == 0.5f);
        CHECK(!s.awake);
    }
}

/* A step past the threshold wakes in one call with no lost travel. */
void TestWakeNoLostTravel()
{
    alchemy::PotState s;
    const float       phys = 0.505f;
    alchemy::UpdateCatch(s, phys, kFrameMs);
    CHECK(s.awake);
    CHECK(s.stored == phys);
}

/* Awake tracks phys exactly — no quantization, no filtering. */
void TestTrackExact()
{
    alchemy::PotState s;
    uint32_t          t = 0;
    alchemy::UpdateCatch(s, 0.51f, t += kFrameMs); /* wake */
    for (int i = 0; i < 100; i++)
    {
        const float phys = 0.51f + 0.003f * static_cast<float>(i);
        alchemy::UpdateCatch(s, phys, t += kFrameMs);
        CHECK(s.stored == phys);
        CHECK(s.awake);
    }
}

/* Full travel over 60 s: never sleeps mid-sweep, ends on target. */
void TestSlowSweepStaysLive()
{
    alchemy::PotState s{0.0f, true, 0};
    uint32_t          t    = 0;
    bool              woke = false;
    for (int i = 0; i <= 3750; i++)
    {
        const float phys = static_cast<float>(i) / 3750.0f;
        alchemy::UpdateCatch(s, phys, t += kFrameMs);
        if (s.awake) woke = true;
        if (woke) CHECK(s.awake);
    }
    CHECK(woke);
    CHECK(s.stored == 1.0f);
}

/* Stillness past kPotSleepMs sleeps; stored then freezes under noise. */
void TestSleepThenFreeze()
{
    alchemy::PotState s;
    uint32_t          t = 0;
    alchemy::UpdateCatch(s, 0.6f, t += kFrameMs); /* wake */
    CHECK(s.awake);
    for (int i = 0; i < 30; i++)
        alchemy::UpdateCatch(s, 0.6f, t += kFrameMs);
    CHECK(!s.awake);

    const float frozen = s.stored;
    Rng         rng;
    for (int i = 0; i < 100; i++)
    {
        alchemy::UpdateCatch(s, 0.6f + 3e-4f * rng.Next(), t += kFrameMs);
        CHECK(s.stored == frozen);
    }
}

/* Post-filter-scale noise must not block re-sleep after a wake. */
void TestResleepUnderNoise()
{
    alchemy::PotState s;
    uint32_t          t = 0;
    alchemy::UpdateCatch(s, 0.6f, t += kFrameMs); /* wake */
    Rng rng;
    for (int i = 0; i < 60; i++)
        alchemy::UpdateCatch(s, 0.6f + 1e-4f * rng.Next(), t += kFrameMs);
    CHECK(!s.awake);
}

/* Sleep timing survives uint32 t_ms wrap-around. */
void TestTimerWrap()
{
    alchemy::PotState s;
    uint32_t          t = 0xFFFFFF00u;
    alchemy::UpdateCatch(s, 0.7f, t); /* wake */
    CHECK(s.awake);
    for (int i = 0; i < 40; i++)
    {
        t += kFrameMs; /* wraps past 0 */
        alchemy::UpdateCatch(s, 0.7f, t);
    }
    CHECK(!s.awake);
}

/* Untimed overload keeps today's semantics: raw follow, no gate. */
void TestLegacyUntimed()
{
    alchemy::PotState s;
    alchemy::UpdateCatch(s, 0.512345f);
    CHECK(s.stored == 0.512345f);
    alchemy::UpdateCatch(s, 0.5121f);
    CHECK(s.stored == 0.5121f);

    alchemy::PotState u{0.5f, false, 1};
    alchemy::UpdateCatch(u, 0.6f); /* same side: still uncaught */
    CHECK(!u.caught);
    alchemy::UpdateCatch(u, 0.4f); /* crossover */
    CHECK(u.caught);
    CHECK(u.stored == 0.4f);
}

/* Proximity and crossover catches enter awake; InitCatch parks asleep. */
void TestCatchTransitions()
{
    alchemy::PotState a{0.5f, false, 1};
    alchemy::UpdateCatch(a, 0.502f, kFrameMs);
    CHECK(a.caught);
    CHECK(a.awake);

    alchemy::PotState b{0.5f, false, 1};
    alchemy::UpdateCatch(b, 0.4f, kFrameMs);
    CHECK(b.caught);
    CHECK(b.awake);

    alchemy::PotState c;
    c.stored = 0.3f;
    alchemy::InitCatch(c, 0.3f);
    CHECK(c.caught);
    CHECK(!c.awake);
    alchemy::InitCatch(c, 0.9f);
    CHECK(!c.caught);
    CHECK(!c.awake);
}

} // namespace

int main()
{
    TestHoldAsleep();
    TestWakeNoLostTravel();
    TestTrackExact();
    TestSlowSweepStaysLive();
    TestSleepThenFreeze();
    TestResleepUnderNoise();
    TestTimerWrap();
    TestLegacyUntimed();
    TestCatchTransitions();

    if (failures)
    {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("pot_gate_test: all tests passed\n");
    return 0;
}
