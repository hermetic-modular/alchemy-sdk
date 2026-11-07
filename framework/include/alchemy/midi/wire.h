#pragma once
#include "alchemy/midi/types.h"

namespace alchemy::midi {
enum class SysExPhase : uint8_t { Begin, Data, End, Abort };
struct SysExChunk {
    SysExPhase phase = SysExPhase::Data;
    SourceId source{};
    uint32_t time_us = 0;
    uint8_t byte = 0;
};
struct SysExSink {
    void* context = nullptr;
    void (*receive)(void*, const SysExChunk&) = nullptr;
};

class StreamDecoder {
  public:
    bool Init(SourceId source) {
        if (source.port == kAnyPort || !source.session ||
            (initialized_ && (source.port != source_.port || !SessionAfter(source.session, source_.session)))) return false;
        Abort(last_time_us_);
        // The abort callback may have initialized a newer session.
        if (initialized_ && (source.port != source_.port || !SessionAfter(source.session, source_.session))) return false;
        source_ = source; initialized_ = true;
        status_ = needed_ = count_ = 0;
        return true;
    }
    void SetSysExSink(SysExSink sink) { if (sysex_) Abort(last_time_us_); sysex_sink_ = sink; }
    void Abort(uint32_t time_us) {
        const bool active = sysex_;
        sysex_ = false; status_ = needed_ = count_ = 0;
        if (active) Emit(SysExPhase::Abort, time_us);
    }
    bool Feed(uint8_t byte, uint32_t time_us, Event& event) {
        if (!initialized_) return false;
        last_time_us_ = time_us;
        if (byte >= 0xf8) {
            EventType type;
            switch (byte) {
                case 0xf8: type = EventType::TimingClock; break;
                case 0xfa: type = EventType::Start; break;
                case 0xfb: type = EventType::Continue; break;
                case 0xfc: type = EventType::Stop; break;
                case 0xfe: type = EventType::ActiveSensing; break;
                case 0xff: type = EventType::SystemReset; break;
                default: ++ignored_; return false;
            }
            event = Event::System(type, source_, time_us);
            return true;
        }
        if (byte & 0x80) {
            if (sysex_ && byte == 0xf7) {
                sysex_ = false;
                Emit(SysExPhase::End, time_us);
                return false;
            }
            const SourceId previous_source = source_;
            if (sysex_) Abort(time_us);
            if (source_ != previous_source) return false;
            sysex_ = false; count_ = needed_ = status_ = 0;
            if (byte < 0xf0) {
                status_ = byte;
                needed_ = ((byte & 0xf0) == 0xc0 || (byte & 0xf0) == 0xd0) ? 1 : 2;
            } else if (byte == 0xf0) {
                sysex_ = true; Emit(SysExPhase::Begin, time_us);
            } else if (byte == 0xf1 || byte == 0xf2 || byte == 0xf3) {
                status_ = byte; needed_ = byte == 0xf2 ? 2 : 1;
            } else if (byte == 0xf6) {
                event = Event::System(EventType::TuneRequest, source_, time_us);
                return true;
            } else { ++ignored_; }
            return false;
        }
        if (sysex_) { Emit(SysExPhase::Data, time_us, byte); return false; }
        if (!needed_) { ++ignored_; return false; }
        data_[count_++] = byte;
        if (count_ != needed_) return false;
        count_ = 0;
        event = Event::System(EventType::NoteOff, source_, time_us);
        if (status_ < 0xf0) {
            event.channel = status_ & 15;
            event.data1 = data_[0]; event.value = data_[1];
            switch (status_ & 0xf0) {
                case 0x80: event.type = EventType::NoteOff; break;
                case 0x90:
                    event.type = data_[1] ? EventType::NoteOn : EventType::NoteOff;
                    if (!data_[1]) event.note_off_origin = NoteOffOrigin::ZeroVelocityNoteOn;
                    break;
                case 0xa0: event.type = EventType::PolyPressure; break;
                case 0xb0: event.type = EventType::ControlChange; break;
                case 0xc0: event.type = EventType::ProgramChange; event.data1 = 0; event.value = data_[0]; break;
                case 0xd0: event.type = EventType::ChannelPressure; event.data1 = 0; event.value = data_[0]; break;
                case 0xe0:
                    event.type = EventType::PitchBend; event.data1 = 0;
                    event.value = uint16_t(data_[0]) | (uint16_t(data_[1]) << 7); event.resolution = 14; break;
            }
        } else {
            if (status_ == 0xf1) { event.type = EventType::TimeCode; event.value = data_[0]; }
            else if (status_ == 0xf2) {
                event.type = EventType::SongPosition; event.value = uint16_t(data_[0]) | (uint16_t(data_[1]) << 7);
                event.resolution = 14;
            } else { event.type = EventType::SongSelect; event.value = data_[0]; }
            status_ = needed_ = 0;
        }
        return true;
    }
    uint32_t Ignored() const { return ignored_; }
    bool InSysEx() const { return sysex_; }
    SourceId Source() const { return source_; }
  private:
    void Emit(SysExPhase phase, uint32_t time_us, uint8_t byte = 0) {
        if (sysex_sink_.receive) sysex_sink_.receive(sysex_sink_.context, {phase, source_, time_us, byte});
    }
    SourceId source_{};
    SysExSink sysex_sink_{};
    uint32_t last_time_us_ = 0, ignored_ = 0;
    uint8_t status_ = 0, needed_ = 0, count_ = 0, data_[2]{};
    bool initialized_ = false, sysex_ = false;
};
}
