#pragma once

#include "alchemy/host_link/frame.h"
#include "alchemy/surface/presets.h"

namespace alchemy::hostlink {

// A byte-for-byte slice of the owner's preset representation. Context must
// outlive Host. Capture must fill exactly size bytes in bounded control-loop
// time. Edit receives a slice-relative offset and must reject invalid edits
// before changing state. A null edit callback makes the slice read-only.
struct FastProjection {
    uint16_t offset;
    uint16_t size;
    void* context;
    void (*capture)(void*, uint8_t*, uint16_t);
    bool (*edit)(void*, uint16_t, const uint8_t*, uint16_t) = nullptr;
};

class FastValues {
  public:
    bool Add(Serializable& owner);
    bool Add(Serializable& owner, FastProjection projection);
    bool Prepare(const Presets& presets, const float* physical_pots, uint8_t pot_count);
    bool Empty() const { return count_ == 0; }
    void Handle(const ParsedFrame& request, FrameWriter& response) const;

  private:
    struct Entry {
        Serializable* owner = nullptr;
        FastProjection projection{};
        uint16_t offset = 0; // absolute offset in the full preset
        uint16_t size = 0;
        bool writable = false;
    };
    bool MatchesLayout() const;
    Status Edit(const ParsedFrame& request) const;
    Entry entries_[16]{};
    uint8_t count_ = 0;
    const Presets* presets_ = nullptr;
    const float* physical_pots_ = nullptr;
    uint32_t schema_ = 0;
};

} // namespace alchemy::hostlink
