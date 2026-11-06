#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace alchemy::midi {

enum class SlideCurve : uint8_t { Linear, Exponential };
enum class SlideTiming : uint8_t { Duration, Rate };

// Configured trajectory; the note policy decides whether an action actually slides.
struct PitchTrajectory {
    SlideCurve curve = SlideCurve::Exponential;
    SlideTiming timing = SlideTiming::Duration;
    float time_ms = 60.f;
    float semitones_per_second = 12.f;
};

struct PitchTransition {
    bool slide = false;
    SlideCurve curve = SlideCurve::Exponential;
    SlideTiming timing = SlideTiming::Duration;
    float time_ms = 60.f;
    float semitones_per_second = 12.f;
};

class PitchSlew {
  public:
    bool Init(float sample_rate, float pitch = 69.f) {
        if (!Finite(sample_rate) || sample_rate <= 0.f || !Finite(pitch)) return false;
        sample_rate_ = sample_rate;
        current_ = start_ = target_ = pitch;
        elapsed_ = frames_ = 0;
        return true;
    }

    bool SetTarget(float pitch, PitchTransition transition = {}) {
        if (!Finite(pitch) || !Finite(transition.time_ms) || transition.time_ms < 0.f ||
            !Finite(transition.semitones_per_second) || transition.semitones_per_second <= 0.f ||
            transition.curve > SlideCurve::Exponential || transition.timing > SlideTiming::Rate)
            return false;
        if (!transition.slide || (transition.timing == SlideTiming::Duration && transition.time_ms == 0.f)) {
            current_ = start_ = target_ = pitch;
            elapsed_ = frames_ = 0;
            return true;
        }
        if (pitch == target_) return true;
        const double seconds = transition.timing == SlideTiming::Duration
            ? static_cast<double>(transition.time_ms) / 1000.0
            : std::fabs(static_cast<double>(pitch) - current_) / transition.semitones_per_second;
        const double frames = std::ceil(seconds * sample_rate_);
        if (frames > std::numeric_limits<uint32_t>::max()) return false;
        start_ = current_;
        target_ = pitch;
        curve_ = transition.curve;
        elapsed_ = 0;
        frames_ = static_cast<uint32_t>(frames);
        inverse_frames_ = frames_ ? 1.0 / frames_ : 0.0;
        exponential_ = 1.0;
        decay_ = frames_ ? std::exp(-4.605170185988092 * inverse_frames_) : 1.0;
        if (!frames_) current_ = target_;
        return true;
    }

    float Process() {
        if (!frames_ || elapsed_ >= frames_) return current_;
        if (++elapsed_ == frames_) return current_ = target_;
        double position = elapsed_ * inverse_frames_;
        if (curve_ == SlideCurve::Exponential) {
            exponential_ *= decay_;
            position = (1.0 - exponential_) / 0.99;
        }
        current_ = static_cast<float>(static_cast<double>(start_) +
                    (static_cast<double>(target_) - start_) * position);
        return current_;
    }

    // Advance a control block without running unused per-sample interpolation.
    float Advance(uint32_t frames) {
        if (!frames || !Active()) return current_;
        const uint32_t remaining = frames_ - elapsed_;
        if (frames >= remaining) { elapsed_ = frames_; return current_ = target_; }
        elapsed_ += frames;
        double position = elapsed_ * inverse_frames_;
        if (curve_ == SlideCurve::Exponential) {
            exponential_ = std::exp(-4.605170185988092 * position);
            position = (1.0 - exponential_) / 0.99;
        }
        current_ = static_cast<float>(static_cast<double>(start_) +
            (static_cast<double>(target_) - start_) * position);
        return current_;
    }

    float Current() const { return current_; }
    float Target() const { return target_; }
    bool Active() const { return elapsed_ < frames_; }

  private:
    static bool Finite(float value) { return std::isfinite(value); }
    float sample_rate_ = 48000.f;
    float current_ = 69.f, start_ = 69.f, target_ = 69.f;
    uint32_t elapsed_ = 0, frames_ = 0;
    double inverse_frames_ = 0.0, exponential_ = 1.0, decay_ = 1.0;
    SlideCurve curve_ = SlideCurve::Exponential;
};

inline float NoteToFrequency(float semitones, float reference_a = 440.f) {
    return reference_a * std::exp2((semitones - 69.f) / 12.f);
}

enum class PitchMerge : uint8_t { Cv, Midi, MidiPlusCv, CvPlusMidi, LastActive };
enum class GateMerge : uint8_t { Cv, Midi, Union };
enum class PitchOwner : uint8_t { None, Cv, Midi };

struct PitchGateConfig {
    PitchMerge pitch = PitchMerge::MidiPlusCv;
    GateMerge gate = GateMerge::Midi;
    float midi_reference = 60.f;
    bool cv_attacks = false;
    bool midi_attacks = true;
    bool owner_return_attack = false;
};

struct PitchGateInput {
    float cv_pitch = 60.f;
    float cv_transpose = 0.f;
    float midi_pitch = 60.f;
    bool cv_gate = false, midi_gate = false;
    bool midi_note_on = false;
    bool midi_attack_amp = false, midi_attack_filter = false;
    uint32_t cv_sequence = 0, midi_sequence = 0;
};

struct PitchGateOutput {
    float pitch = 60.f;
    bool gate = false, attack_amp = false, attack_filter = false;
    PitchOwner owner = PitchOwner::None;
};

class PitchGateArbiter {
  public:
    bool Init(PitchGateConfig config = {}) {
        if (config.pitch > PitchMerge::LastActive || config.gate > GateMerge::Union ||
            !std::isfinite(config.midi_reference)) return false;
        config_ = config;
        previous_cv_ = false;
        owner_ = PitchOwner::None;
        last_pitch_ = 60.f;
        return true;
    }

    PitchGateOutput Process(const PitchGateInput& in) {
        const bool cv_attack = in.cv_gate && !previous_cv_;
        previous_cv_ = in.cv_gate;
        const PitchOwner previous_owner = owner_;
        if (cv_attack && in.midi_note_on) {
            owner_ = static_cast<int32_t>(in.midi_sequence - in.cv_sequence) >= 0
                ? PitchOwner::Midi : PitchOwner::Cv;
        } else if (in.midi_note_on) owner_ = PitchOwner::Midi;
        else if (cv_attack) owner_ = PitchOwner::Cv;
        bool returned = false;
        if (owner_ == PitchOwner::Midi && !in.midi_gate && in.cv_gate) {
            owner_ = PitchOwner::Cv; returned = true;
        } else if (owner_ == PitchOwner::Cv && !in.cv_gate && in.midi_gate) {
            owner_ = PitchOwner::Midi; returned = true;
        }
        if (!in.cv_gate && !in.midi_gate) owner_ = PitchOwner::None;
        else if (owner_ == PitchOwner::None) {
            owner_ = !in.cv_gate ? PitchOwner::Midi : !in.midi_gate ? PitchOwner::Cv
                : static_cast<int32_t>(in.midi_sequence-in.cv_sequence)>=0 ? PitchOwner::Midi : PitchOwner::Cv;
        }

        PitchGateOutput out;
        out.owner = owner_;
        switch (config_.pitch) {
        case PitchMerge::Cv: out.pitch = in.cv_pitch; break;
        case PitchMerge::Midi: out.pitch = in.midi_pitch; break;
        case PitchMerge::MidiPlusCv:
            out.pitch = CombinedPitch(static_cast<double>(in.midi_pitch) + in.cv_transpose); break;
        case PitchMerge::CvPlusMidi:
            out.pitch = CombinedPitch(static_cast<double>(in.cv_pitch) + in.midi_pitch - config_.midi_reference);
            break;
        case PitchMerge::LastActive:
            out.pitch = owner_ == PitchOwner::Cv ? in.cv_pitch
                      : owner_ == PitchOwner::Midi ? in.midi_pitch : last_pitch_;
            break;
        }
        if (!std::isfinite(out.pitch)) out.pitch = last_pitch_;
        last_pitch_ = out.pitch;
        out.gate = config_.gate == GateMerge::Cv ? in.cv_gate
                 : config_.gate == GateMerge::Midi ? in.midi_gate : in.cv_gate || in.midi_gate;
        const bool owner_attack = config_.pitch == PitchMerge::LastActive &&
            config_.owner_return_attack && returned && previous_owner != PitchOwner::None &&
            owner_ != previous_owner && out.gate;
        out.attack_amp = out.gate && (owner_attack || (config_.cv_attacks && cv_attack) ||
                          (config_.midi_attacks && in.midi_attack_amp));
        out.attack_filter = out.gate && (owner_attack || (config_.cv_attacks && cv_attack) ||
                             (config_.midi_attacks && in.midi_attack_filter));
        return out;
    }

  private:
    float CombinedPitch(double value) const {
        return std::isfinite(value) && std::fabs(value) <= std::numeric_limits<float>::max()
            ? static_cast<float>(value) : last_pitch_;
    }
    PitchGateConfig config_{};
    bool previous_cv_ = false;
    PitchOwner owner_ = PitchOwner::None;
    float last_pitch_ = 60.f;
};

} // namespace alchemy::midi
