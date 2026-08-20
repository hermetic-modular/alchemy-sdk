/**
 * @file alchemy/surface/see_ref.h
 * @brief alchemy::SeeRef — typed manual cross-reference (.SeeAlso targets).
 *
 * Holds the referenced object, not a string id; the descriptor build
 * resolves and validates it, so a dead link is a build failure.  Knobs and
 * settings handles must carry .Ident() to be referenced; buttons and jacks
 * always resolve.  The raw-string form ("presets") covers non-object
 * entities.  Storage only — resolution lives in the builder.
 */

#pragma once

#include <cstdint>

namespace alchemy {

class VirtualKnob;
class VirtualButton;
class Jack;
struct SettingsSlot;

/** Max SeeAlso refs per entity; overflow fails the descriptor build. */
constexpr uint8_t kMaxSeeRefs = 4u;

class SeeRef
{
  public:
    enum class Kind : uint8_t { None, Raw, Knob, Button, Jack, Slot };

    constexpr SeeRef() = default;
    constexpr SeeRef(const char* raw_id) : kind_(Kind::Raw), raw_(raw_id) {}
    constexpr SeeRef(const VirtualKnob& k) : kind_(Kind::Knob), ptr_(&k) {}
    constexpr SeeRef(const VirtualButton& b) : kind_(Kind::Button), ptr_(&b) {}
    constexpr SeeRef(const Jack& j) : kind_(Kind::Jack), ptr_(&j) {}
    constexpr SeeRef(const SettingsSlot& s) : kind_(Kind::Slot), ptr_(&s) {}

    constexpr Kind        K()   const { return kind_; }
    constexpr const void* Ptr() const { return ptr_; }
    constexpr const char* Raw() const { return raw_; }

  private:
    Kind        kind_ = Kind::None;
    const void* ptr_  = nullptr;
    const char* raw_  = nullptr;
};

} // namespace alchemy
