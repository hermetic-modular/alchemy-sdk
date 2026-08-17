/**
 * @file daisy_seed.h (host-build stub)
 *
 * Minimal libDaisy type stubs for compiling the alchemy-sdk on a host
 * machine without the ARM cross-toolchain.  Provides the same public API
 * surface used by the SDK; all method bodies are no-ops or return safe
 * sentinel values.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include "daisy_core.h"

namespace daisy {

/* ── Pin ──────────────────────────────────────────────────────────────── */

using Pin = uint32_t;

namespace seed {
    static constexpr Pin D0  =  0, D1  =  1, D2  =  2, D3  =  3;
    static constexpr Pin D4  =  4, D5  =  5, D6  =  6, D7  =  7;
    static constexpr Pin D8  =  8, D9  =  9, D10 = 10, D11 = 11;
    static constexpr Pin D12 = 12, D13 = 13, D14 = 14, D15 = 15;
    static constexpr Pin D16 = 16, D17 = 17, D18 = 18, D19 = 19;
    static constexpr Pin D20 = 20, D21 = 21, D22 = 22, D23 = 23;
    static constexpr Pin D24 = 24, D25 = 25, D26 = 26, D27 = 27;
    static constexpr Pin D28 = 28, D29 = 29, D30 = 30, D31 = 31;
    static constexpr Pin D32 = 32;
} // namespace seed

/* ── SaiHandle ────────────────────────────────────────────────────────── */

struct SaiHandle {
    struct Config {
        enum class SampleRate : uint8_t {
            SAI_8KHZ  = 0,
            SAI_16KHZ = 1,
            SAI_32KHZ = 2,
            SAI_48KHZ = 3,
            SAI_96KHZ = 4,
        };
    };
};

/* ── AudioHandle ──────────────────────────────────────────────────────── */

struct AudioHandle {
    using InputBuffer   = const float* const*;
    using OutputBuffer  = float**;
    using AudioCallback = void (*)(InputBuffer, OutputBuffer, size_t);
};

/* ── AdcChannelConfig ─────────────────────────────────────────────────── */

struct AdcChannelConfig {
    void InitSingle(Pin) {}
};

/* ── AdcHandle ────────────────────────────────────────────────────────── */

struct AdcHandle {
    void   Init(AdcChannelConfig*, uint8_t) {}
    void   Start()                            {}
    uint16_t* GetPtr(uint8_t)                 { return nullptr; }
    float  GetFloat(uint8_t)                  { return 0.f; }
};

/* ── QSPIHandle ───────────────────────────────────────────────────────── */

struct QSPIHandle {
    enum class Result { OK, ERR };
    Result Erase(uint32_t, uint32_t)              { return Result::OK; }
    Result Write(uint32_t, uint32_t, uint8_t*)    { return Result::OK; }
};

/* ── AnalogControl ────────────────────────────────────────────────────── */

/* Functional, not a no-op: host tests drive CvInput's transfer math
 * through this. Instantaneous (slew ignored — the SDK's CV paths pass
 * slew = 0). Matches the real control's documented semantics:
 * Value() = raw/65535, flip → 1 − x, invert → 1 − x after flip.
 * Callers that Init with a null ADC pointer keep the old sentinel 0. */
struct AnalogControl {
    void Init(uint16_t* adcptr, float, bool flip, bool invert, float)
    {
        raw_    = adcptr;
        flip_   = flip;
        invert_ = invert;
        val_    = 0.f;
    }
    void Process()
    {
        if (!raw_) return;
        float t = static_cast<float>(*raw_) / 65535.f;
        if (flip_)   t = 1.f - t;
        if (invert_) t = 1.f - t;
        val_ = t;
    }
    float Value() const { return val_; }

  private:
    uint16_t* raw_    = nullptr;
    bool      flip_   = false;
    bool      invert_ = false;
    float     val_    = 0.f;
};

/* ── Switch ───────────────────────────────────────────────────────────── */

struct Switch {
    void  Init(Pin)            {}
    void  Debounce()           {}
    bool  RisingEdge()  const  { return false; }
    bool  FallingEdge() const  { return false; }
    bool  Pressed()     const  { return false; }
    float TimeHeldMs()  const  { return 0.f; }
};

/* ── System ───────────────────────────────────────────────────────────── */

struct System {
    static uint32_t GetNow()       { return 0; }
    static uint32_t GetUs()        { return 0; }
    static void     Delay(uint32_t) {}
};

/* ── DaisySeed ────────────────────────────────────────────────────────── */

struct DaisySeed {
    AdcHandle  adc;
    QSPIHandle qspi;

    void Configure() {}
    void Init()      {}

    void SetAudioBlockSize(uint32_t)                       {}
    void SetAudioSampleRate(SaiHandle::Config::SampleRate) {}
    void StartAudio(AudioHandle::AudioCallback)            {}

    float  AudioSampleRate()        { return 48000.f; }
    float  AudioCallbackRate() const { return 3000.f; }
    size_t AudioBlockSize()         { return 16u; }
};

} // namespace daisy
