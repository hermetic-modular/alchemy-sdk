/**
 * @file alchemy/surface/serializable.h
 * @brief alchemy::Serializable — abstract byte-stream contract for things
 *        that want to ride along with a Presets save/load.
 *
 * A Serializable advertises its byte size, writes its bytes into a caller
 * buffer, restores its state from a caller buffer, and reports a schema
 * hash that captures the shape of its layout.  Presets walks an ordered
 * list of Serializable* on every Save/Load; the order is the on-flash
 * byte layout.
 *
 * Aggregating the SchemaHash of every managed Serializable produces a
 * single hash stamped into each slot.  On Load, a mismatch means the
 * firmware's framework layout has changed since that slot was written —
 * the slot is treated as empty rather than risking a misaligned restore.
 *
 * SchemaHash is *not* a version number.  Two builds with the same managed
 * shape produce the same hash and read each other's slots; two builds
 * that differ in (say) ParamLock slot count produce different hashes and
 * their slots are mutually invisible.  Bump intentionally only when you
 * change a layout in a way that should invalidate existing data.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace alchemy {

namespace hostlink { class ComponentWriter; }

class Serializable
{
  public:
    virtual ~Serializable() = default;

    /** Component archetype, used by HostLink descriptor auto-derivation
     *  to recognize the standard surfaces.  User components keep the
     *  Opaque default and may implement Describe() instead. */
    enum class DescKind : uint8_t { Opaque, Pager, Settings, Name };

    virtual DescKind DescribeKind() const { return DescKind::Opaque; }

    /**
     * Optional self-description for HostLink hosts (see
     * alchemy/host_link/component_writer.h).  Emit this component's
     * editable fields into @p w and return true; return false — writing
     * NOTHING to @p w — to be treated as an opaque byte blob (the
     * default).  Only consulted when the firmware links HostLink.
     */
    virtual bool Describe(hostlink::ComponentWriter& w) const
    {
        (void)w;
        return false;
    }

    /** Number of bytes this object will write into Serialize(). */
    virtual size_t SerializedSize() const = 0;

    /** Write SerializedSize() bytes into @p out. */
    virtual void Serialize(uint8_t* out) const = 0;

    /**
     * Read SerializedSize() bytes from @p in and apply.
     *
     * Implementations may defer hardware-visible work (re-arming pot
     * catches, calling SetBrightness) to the next per-frame Update —
     * Presets has no notion of physical pot values.
     *
     * @return false if the bytes are obviously malformed.  Presets does
     *         not act on this beyond reporting load failure.
     */
    virtual bool Deserialize(const uint8_t* in) = 0;

    /**
     * 32-bit hash describing this object's serialized layout.
     *
     * Must be stable across builds when the layout is unchanged, and
     * must differ when the layout differs.  XOR-combined with every
     * other managed Serializable's hash to form the slot's schema hash.
     */
    virtual uint32_t SchemaHash() const = 0;
};

} // namespace alchemy
