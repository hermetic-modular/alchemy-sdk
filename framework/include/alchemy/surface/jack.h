/**
 * @file alchemy/surface/jack.h
 * @brief alchemy::Jack — pure descriptor metadata for a panel jack.
 */

#pragma once

#include <cstdint>

#include "alchemy/surface/see_ref.h"

namespace alchemy {

/** Closed signal vocabulary (wire strings via Jack::SigName). */
enum class JackSig : uint8_t
{
    AudioIn,  //  "audio-in"   → host renders "Audio In"
    AudioOut, //  "audio-out"  → "Audio Out"
    CvBi,     //  "cv-bi"      → "±5 V"
    CvUni,    //  "cv-uni"     → "0–5 V"
    Voct,     //  "voct"       → "1 V/oct"
    Gate,     //  "gate"       → "Gate"
    Trig,     //  "trig"       → "Trig"
};

class Jack
{
  public:
    constexpr Jack(const char* id, const char* name, JackSig sig)
        : id_(id), name_(name), sig_(sig) {}

    /** Short silk label for panel drawings; hosts fall back to name. */
    constexpr Jack& Short(const char* silk) { short_ = silk; return *this; }

    /** Input normalled from @p from when unpatched. */
    constexpr Jack& Normalled(const Jack& from) { norm_ = &from; return *this; }

    constexpr Jack& Help(const char* md) { help_ = md; return *this; }

    template <typename... Refs>
    constexpr Jack& SeeAlso(const Refs&... refs)
    {
        (AddSee(SeeRef(refs)), ...);
        return *this;
    }

    constexpr const char* Id()    const { return id_; }
    constexpr const char* Name()  const { return name_; }
    constexpr const char* Silk()  const { return short_; }
    constexpr JackSig     Sig()   const { return sig_; }
    constexpr const Jack* Norm()  const { return norm_; }
    constexpr const char* ManualHelp() const { return help_; }
    constexpr const SeeRef* SeeRefs() const { return see_; }
    constexpr uint8_t NumSeeRefs() const { return num_see_; }
    constexpr bool    SeeOverflowed() const { return see_overflow_; }

    /** Wire string for a signal class. */
    static constexpr const char* SigName(JackSig s)
    {
        switch (s)
        {
            case JackSig::AudioIn:  return "audio-in";
            case JackSig::AudioOut: return "audio-out";
            case JackSig::CvBi:     return "cv-bi";
            case JackSig::CvUni:    return "cv-uni";
            case JackSig::Voct:     return "voct";
            case JackSig::Gate:     return "gate";
            case JackSig::Trig:     /* fall through */
            default:                return "trig";
        }
    }

  private:
    constexpr void AddSee(const SeeRef& r)
    {
        if (num_see_ < kMaxSeeRefs) see_[num_see_++] = r;
        else                        see_overflow_ = true;
    }

    const char* id_;
    const char* name_;
    const char* short_ = nullptr;
    JackSig     sig_;
    const Jack* norm_  = nullptr;

    const char* help_  = nullptr;
    SeeRef      see_[kMaxSeeRefs] = {};
    uint8_t     num_see_          = 0u;
    bool        see_overflow_     = false;
};

} // namespace alchemy
