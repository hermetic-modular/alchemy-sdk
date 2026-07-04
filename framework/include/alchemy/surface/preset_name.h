/**
 * @file alchemy/surface/preset_name.h
 * @brief alchemy::PresetName — a preset's display name, stored in the
 *        preset blob itself so names travel with the hardware.
 *
 * A fixed NUL-padded buffer (kCapacity bytes, so kCapacity−1 usable
 * characters).  The firmware carries it but never displays it (LED-only
 * panels); hosts read and edit it through the HostLink descriptor's
 * "name" component.
 *
 * Manage() it LAST so adding it to an existing firmware leaves every
 * other component's blob offset unchanged (the schema hash still
 * changes — adding this component invalidates previously saved slots).
 */

#pragma once

#include <cstring>
#include "alchemy/surface/serializable.h"

namespace alchemy {

class PresetName : public Serializable
{
  public:
    /** Total bytes on flash, including the guaranteed NUL. */
    static constexpr size_t kCapacity = 24u;

    PresetName() { std::memset(buf_, 0, kCapacity); }

    const char* Get() const { return buf_; }

    void Set(const char* s)
    {
        std::memset(buf_, 0, kCapacity);
        if (!s) return;
        size_t i = 0;
        for (; s[i] != '\0' && i < kCapacity - 1u; i++) buf_[i] = s[i];
    }

    /* ── Serializable ──────────────────────────────────────────────── */

    size_t SerializedSize() const override { return kCapacity; }

    void Serialize(uint8_t* out) const override
    {
        std::memcpy(out, buf_, kCapacity);
    }

    bool Deserialize(const uint8_t* in) override
    {
        std::memcpy(buf_, in, kCapacity);
        buf_[kCapacity - 1u] = '\0';   /* terminate whatever flash held */
        return true;
    }

    uint32_t SchemaHash() const override
    {
        return 0x4E414D45u /* 'NAME' */ ^ static_cast<uint32_t>(kCapacity);
    }

    DescKind DescribeKind() const override { return DescKind::Name; }

  private:
    char buf_[kCapacity];
};

} // namespace alchemy
