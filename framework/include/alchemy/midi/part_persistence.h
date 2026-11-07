#pragma once

#include <algorithm>
#include "alchemy/midi/detail/codec.h"
#include "alchemy/midi/note_part.h"
#include "alchemy/surface/serializable.h"

namespace alchemy::midi {

class PartCodec : private detail::Codec {
  public:
    static constexpr size_t kSize = 64;
    static constexpr uint16_t kVersion = 2;
    static constexpr uint32_t kSchema = 0x50544d02u;

    static bool Encode(const PartConfig& config, uint8_t* output, size_t size) {
        if (!output || size != kSize || !config.Valid()) return false;
        std::fill(output, output + size, uint8_t{0});
        Put32(output, 0x50544d41u); Put16(output + 4, kVersion); Put16(output + 6, kSize);
        Put16(output + 16, config.listen.channel_mask); Put16(output + 18, config.listen.port);
        output[20] = static_cast<uint8_t>(config.mode);
        output[21] = static_cast<uint8_t>(config.priority);
        output[22] = static_cast<uint8_t>(config.duplicates);
        output[23] = static_cast<uint8_t>(config.allocation);
        output[24] = static_cast<uint8_t>(config.steal);
        output[25] = static_cast<uint8_t>(config.articulation.amp);
        output[26] = static_cast<uint8_t>(config.articulation.filter);
        output[27] = static_cast<uint8_t>(config.articulation.onset);
        output[28] = static_cast<uint8_t>(config.slide.trigger);
        output[29] = static_cast<uint8_t>(config.slide.transition.curve);
        output[30] = static_cast<uint8_t>(config.slide.transition.timing);
        output[31] = static_cast<uint8_t>(config.shared_amp_attack);
        output[32] = static_cast<uint8_t>(config.shared_filter_attack);
        output[33] = static_cast<uint8_t>(config.shared_velocity);
        output[34] = static_cast<uint8_t>((config.sustain ? 1 : 0) | (config.sostenuto ? 2 : 0)
            | (config.enable_mpe ? 4 : 0) | (config.slide.return_to_held ? 8 : 0)
            | (config.high_resolution_velocity ? 32 : 0));
        output[35] = static_cast<uint8_t>(config.system_reset_scope);
        PutFloat(output + 36, config.bend_range); PutFloat(output + 40, config.fixed_velocity);
        PutFloat(output + 44, config.slide.transition.time_ms);
        PutFloat(output + 48, config.slide.transition.semitones_per_second);
        Put32(output + 8, Crc(output, kSize, 8));
        return true;
    }

    static bool Decode(const uint8_t* input, size_t size, PartConfig& config) {
        if (!input || size != kSize || Get32(input) != 0x50544d41u || Get16(input + 4) != kVersion
            || Get16(input + 6) != kSize || Get32(input + 8) != Crc(input, kSize, 8) || (input[34] & ~uint8_t{47})
            || input[35] > static_cast<uint8_t>(SystemResetScope::Ignore)) return false;
        for (size_t i = 12; i < 16; ++i) if (input[i]) return false;
        for (size_t i = 52; i < kSize; ++i) if (input[i]) return false;
        PartConfig staged;
        staged.listen = {Get16(input + 16), Get16(input + 18)};
        staged.mode = static_cast<NoteMode>(input[20]);
        staged.priority = static_cast<NotePriority>(input[21]);
        staged.duplicates = static_cast<DuplicatePolicy>(input[22]);
        staged.allocation = static_cast<Allocation>(input[23]);
        staged.steal = static_cast<StealPolicy>(input[24]);
        staged.articulation = {static_cast<AttackPolicy>(input[25]), static_cast<AttackPolicy>(input[26]),
                              static_cast<OnsetPolicy>(input[27])};
        staged.slide.trigger = static_cast<SlideTrigger>(input[28]);
        staged.slide.transition.curve = static_cast<SlideCurve>(input[29]);
        staged.slide.transition.timing = static_cast<SlideTiming>(input[30]);
        staged.shared_amp_attack = static_cast<AttackPolicy>(input[31]);
        staged.shared_filter_attack = static_cast<AttackPolicy>(input[32]);
        staged.shared_velocity = static_cast<SharedVelocity>(input[33]);
        staged.sustain = input[34] & 1; staged.sostenuto = input[34] & 2;
        staged.enable_mpe = input[34] & 4; staged.slide.return_to_held = input[34] & 8;
        staged.system_reset_scope = static_cast<SystemResetScope>(input[35]);
        staged.high_resolution_velocity = input[34] & 32;
        staged.bend_range = GetFloat(input + 36); staged.fixed_velocity = GetFloat(input + 40);
        staged.slide.transition.time_ms = GetFloat(input + 44);
        staged.slide.transition.semitones_per_second = GetFloat(input + 48);
        if (!staged.Valid()) return false;
        config = staged;
        return true;
    }

};

class PartSerializable final : public alchemy::Serializable {
  public:
    explicit PartSerializable(PartConfig& config) : config_(config) {}
    bool CanSerialize() const { return config_.Valid(); }
    size_t SerializedSize() const override { return PartCodec::kSize; }
    void Serialize(uint8_t* output) const override {
        if (output && !PartCodec::Encode(config_, output, SerializedSize()))
            std::fill(output, output + SerializedSize(), uint8_t{0});
    }
    bool Deserialize(const uint8_t* input) override { return PartCodec::Decode(input, SerializedSize(), config_); }
    uint32_t SchemaHash() const override { return PartCodec::kSchema; }
  private:
    PartConfig& config_;
};

} // namespace alchemy::midi
