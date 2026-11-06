#include "alchemy/host_link/fast_values.h"
#include "alchemy/surface/pager.h"
#include "alchemy/surface/settings.h"
#include <algorithm>
#include <cstring>

namespace alchemy::hostlink {

bool FastValues::Add(Serializable& owner) {
    if (count_ == 16 || presets_) return false;
    entries_[count_++].owner = &owner;
    return true;
}

bool FastValues::Add(Serializable& owner, FastProjection projection) {
    if (!projection.size || !projection.capture || !Add(owner)) return false;
    entries_[count_-1].projection = projection;
    return true;
}

bool FastValues::Prepare(const Presets& presets, const float* physical_pots, uint8_t pot_count) {
    presets_ = nullptr;
    if (Empty() || presets.LiveSize() > UINT16_MAX) return false;
    size_t frame_size = 5; // status + schema
    for (unsigned i = 0; i < count_; ++i) {
        auto& entry = entries_[i];
        size_t base = 0;
        unsigned j = 0;
        for (; j < presets.NumManaged() && presets.ManagedAt(j) != entry.owner; ++j)
            base += presets.ManagedAt(j)->SerializedSize();
        if (j == presets.NumManaged()) return false;
        const auto& view = entry.projection;
        const size_t total = entry.owner->SerializedSize();
        const size_t size = view.capture ? view.size : total;
        if (!size || view.offset > total || size > total - view.offset) return false;
        entry.offset = static_cast<uint16_t>(base + view.offset);
        entry.size = static_cast<uint16_t>(size);
        const auto kind = entry.owner->DescribeKind();
        entry.writable = view.capture ? view.edit != nullptr :
            physical_pots && ((kind == Serializable::DescKind::Pager &&
                               static_cast<Pager*>(entry.owner)->NumPots() <= pot_count) ||
                              (kind == Serializable::DescKind::Settings && kNumPots <= pot_count));
        frame_size += 5 + size; // offset + size + writable flag + payload
        if (frame_size > kMaxBody) return false;
    }
    std::sort(entries_, entries_ + count_, [](const Entry& a, const Entry& b) {
        return a.offset < b.offset;
    });
    for (unsigned i = 1; i < count_; ++i)
        if (entries_[i-1].offset + entries_[i-1].size > entries_[i].offset) return false;
    physical_pots_ = physical_pots;
    schema_ = presets.LiveSchemaHash();
    presets_ = &presets;
    return true;
}

bool FastValues::MatchesLayout() const {
    if (!presets_ || presets_->LiveSchemaHash() != schema_) return false;
    // Custom serializers don't take a destination capacity. Recheck selected
    // spans before handing out pointers, even if a faulty component kept its hash.
    size_t base = 0;
    unsigned selected = 0;
    for (unsigned i = 0; i < presets_->NumManaged(); ++i) {
        const auto* owner = presets_->ManagedAt(i);
        const size_t size = owner->SerializedSize();
        if (size > UINT16_MAX - base) return false;
        while (selected < count_ && entries_[selected].owner == owner) {
            const auto& e = entries_[selected++];
            if (e.offset != base + e.projection.offset ||
                e.size != (e.projection.capture ? e.projection.size : size) ||
                e.projection.offset + e.size > size) return false;
        }
        base += size;
    }
    return selected == count_;
}

Status FastValues::Edit(const ParsedFrame& f) const {
    if (f.len <= 6) return Status::BadArgs;
    if (RdU32(f.body) != schema_) return Status::SchemaMismatch;
    const unsigned offset = RdU16(f.body + 4);
    const auto* data = f.body + 6;
    const unsigned length = f.len - 6;
    for (unsigned i = 0; i < count_; ++i) {
        const auto& e = entries_[i];
        if (offset < e.offset || offset + length > unsigned(e.offset) + e.size) continue;
        if (!e.writable) return Status::Unsupported;
        const uint16_t relative = static_cast<uint16_t>(offset - e.offset);
        if (e.projection.edit)
            return e.projection.edit(e.projection.context, relative, data, length)
                ? Status::Ok : Status::BadArgs;
        if (e.owner->DescribeKind() == Serializable::DescKind::Settings)
            return static_cast<Settings*>(e.owner)->SetSerializedField(relative, data, length, physical_pots_)
                ? Status::Ok : Status::BadArgs;
        auto& pager = *static_cast<Pager*>(e.owner);
        if (relative % sizeof(float) || length != sizeof(float)) return Status::BadArgs;
        float value;
        std::memcpy(&value, data, sizeof value);
        if (!(value >= 0 && value <= 1)) return Status::BadArgs;
        const unsigned cell = relative / sizeof(float);
        pager.SetStored(cell / pager.NumPots(), cell % pager.NumPots(), value, physical_pots_);
        return Status::Ok;
    }
    return Status::BadArgs;
}

void FastValues::Handle(const ParsedFrame& f, FrameWriter& w) const {
    if (!MatchesLayout()) { w.U8(uint8_t(Status::SchemaMismatch)); return; }
    if (f.type == uint8_t(Cmd::SetFast)) { w.U8(uint8_t(Edit(f))); return; }
    if (f.len) { w.U8(uint8_t(Status::BadArgs)); return; }
    w.U8(uint8_t(Status::Ok));
    w.U32(schema_);
    for (unsigned i = 0; i < count_; ++i) {
        const auto& e = entries_[i];
        w.U16(e.offset);
        w.U16(e.size);
        w.U8(e.writable ? 1u : 0u);
        auto* out = w.ReserveBytes(e.size);
        if (!out) return;
        if (e.projection.capture) e.projection.capture(e.projection.context, out, e.size);
        else e.owner->Serialize(out);
    }
}

} // namespace alchemy::hostlink
