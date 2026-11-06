#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include "alchemy/midi/types.h"
#include "alchemy/midi/queue.h"

namespace alchemy::midi {
struct ShortMessage {
    std::array<uint8_t, 3> bytes{};
    uint8_t size = 0;
};
enum class TxResult : uint8_t { Accepted, Busy, Invalid, Unsupported, Disconnected, Error };

inline TxResult Validate(const ShortMessage& message) {
    if (!message.size || message.size > 3 || message.bytes[0] < 0x80) return TxResult::Invalid;
    const uint8_t status = message.bytes[0];
    uint8_t expected = 1;
    if (status < 0xf0) expected = (status & 0xf0) == 0xc0 || (status & 0xf0) == 0xd0 ? 2 : 3;
    else if (status == 0xf1 || status == 0xf3) expected = 2;
    else if (status == 0xf2) expected = 3;
    else if (status != 0xf6 && status != 0xf8 && status != 0xfa && status != 0xfb &&
             status != 0xfc && status != 0xfe && status != 0xff) return TxResult::Unsupported;
    if (message.size != expected) return TxResult::Invalid;
    for (uint8_t i = 1; i < expected; ++i) if (message.bytes[i] >= 128) return TxResult::Invalid;
    return TxResult::Accepted;
}


inline TxResult Encode(const Event& e, ShortMessage& m) {
    m = {};
    if (!e.Valid()) return TxResult::Invalid;
    if ((e.type == EventType::NoteOn || e.type == EventType::NoteOff) && e.resolution == 14)
        return TxResult::Unsupported;
    uint8_t status = 0;
    bool channel = true;
    bool one_data = false;
    bool fourteen = false;
    switch (e.type) {
        case EventType::NoteOff:
            if (e.note_off_origin == NoteOffOrigin::ZeroVelocityNoteOn) {
                status = 0x90;
            } else { status = 0x80; }
            break;
        case EventType::NoteOn: status = 0x90; break;
        case EventType::PolyPressure: status = 0xa0; break;
        case EventType::ControlChange: status = 0xb0; break;
        case EventType::ProgramChange: status = 0xc0; one_data = true; break;
        case EventType::ChannelPressure: status = 0xd0; one_data = true; break;
        case EventType::PitchBend: status = 0xe0; fourteen = true; break;
        case EventType::TimeCode: status = 0xf1; channel = false; one_data = true; break;
        case EventType::SongPosition: status = 0xf2; channel = false; fourteen = true; break;
        case EventType::SongSelect: status = 0xf3; channel = false; one_data = true; break;
        case EventType::TuneRequest: status = 0xf6; channel = false; break;
        case EventType::TimingClock: status = 0xf8; channel = false; break;
        case EventType::Start: status = 0xfa; channel = false; break;
        case EventType::Continue: status = 0xfb; channel = false; break;
        case EventType::Stop: status = 0xfc; channel = false; break;
        case EventType::ActiveSensing: status = 0xfe; channel = false; break;
        case EventType::SystemReset: status = 0xff; channel = false; break;
        default: return TxResult::Unsupported;
    }
    m.bytes[0] = static_cast<uint8_t>(status | (channel ? e.channel : 0));
    if (fourteen) {
        m.bytes[1] = static_cast<uint8_t>(e.value & 127);
        m.bytes[2] = static_cast<uint8_t>(e.value >> 7);
        m.size = 3;
    } else if (one_data) {
        m.bytes[1] = static_cast<uint8_t>(e.value);
        m.size = 2;
    } else if (channel) {
        m.bytes[1] = e.data1;
        m.bytes[2] = static_cast<uint8_t>(e.value);
        m.size = 3;
    } else { m.size = 1; }
    return TxResult::Accepted;
}

// Submit callbacks consume or copy messages before returning Accepted.
// generation may be called by either the producer or the consumer.
struct OutputSink {
    void* context = nullptr;
    TxResult (*submit)(void*, const ShortMessage&, uint32_t) = nullptr;
    size_t max_submissions_per_poll = 8;
    TxResult (*submit_bundle)(void*, const ShortMessage*, size_t, uint32_t) = nullptr;
    uint32_t (*generation)(void*) = nullptr;
    size_t max_bundle_messages = 0; // Zero means no declared atomic-bundle limit.
};

// One producer submits; the foreground consumer schedules and transmits.
// Scheduled times must be within half the uint32_t clock range of Poll's time.
template<size_t Capacity, size_t MaxBundle = 8> class Output {
    static_assert(MaxBundle >= 2 && MaxBundle <= 255, "Invalid bundle size");
    struct Group {
        std::array<ShortMessage, MaxBundle> messages{};
        uint32_t deadline = 0, order = 0, generation = 0;
        uint8_t count = 0, position = 0;
        bool scheduled = false, realtime = false;
    };
    struct Heap {
        std::array<size_t, Capacity> indices{};
        size_t size = 0;
    };
  public:
    explicit Output(OutputSink sink = {}) : sink_(sink) {
        for (size_t i = 0; i < Capacity; ++i) free_[i] = Capacity - 1 - i;
        generation_ = Generation();
    }
    // Replacing the destination requires a quiesced producer.
    void SetSink(OutputSink sink) { Clear(); sink_ = sink; generation_ = Generation(); }
    TxResult Send(const Event& e) { return Enqueue(&e, 1, e.time_us, e.time_us != 0); }
    TxResult Schedule(const Event& e, uint32_t deadline) { return Enqueue(&e, 1, deadline, true); }
    TxResult SendBundle(const Event* events, size_t count, uint32_t deadline = 0) {
        return Enqueue(events, count, deadline, deadline != 0);
    }
    TxResult Poll(uint32_t now_us = 0, size_t budget = 8) {
        if (!sink_.submit) return TxResult::Unsupported;
        if (budget > sink_.max_submissions_per_poll) budget = sink_.max_submissions_per_poll;
        const auto generation = Generation();
        if (generation_ != generation) { CancelStaged(); generation_ = generation; }
        poll_time_us_ = now_us;
        Group group;
        for (size_t i = 0; i < Capacity && incoming_.Pop(group); ++i) {
            if (group.generation != generation_) { Cancel(group.count); continue; }
            if (!group.scheduled) group.deadline = now_us;
            const auto slot = free_[--free_count_];
            groups_[slot] = group;
            Insert(group.realtime ? realtime_ : ordinary_, slot);
        }
        for (size_t i = 0; i < budget; ++i) {
            size_t slot = Capacity;
            if (Due(realtime_, now_us)) slot = realtime_.indices[0];
            else if (active_ != Capacity) slot = active_;
            else if (Due(ordinary_, now_us)) {
                active_ = Remove(ordinary_); slot = active_;
            }
            if (slot == Capacity) return TxResult::Accepted;
            auto& current = groups_[slot];
            const size_t remaining = current.count - current.position;
            const bool bundle = sink_.submit_bundle && remaining > 1;
            const auto result = bundle ? sink_.submit_bundle(sink_.context,current.messages.data()+current.position,remaining,current.generation)
                : sink_.submit(sink_.context,current.messages[current.position],current.generation);
            if (result != TxResult::Accepted) {
                if (result == TxResult::Invalid || result == TxResult::Unsupported) {
                    Cancel(remaining);
                    if (current.realtime) Remove(realtime_);
                    else active_ = Capacity;
                    free_[free_count_++] = slot;
                }
                return result;
            }
            const auto sent = bundle ? remaining : 1;
            submitted_ += uint32_t(sent);
            outstanding_.fetch_sub(uint32_t(sent), std::memory_order_release);
            current.position += uint8_t(sent);
            if (current.position == current.count) {
                if (current.realtime) Remove(realtime_);
                else active_ = Capacity;
                free_[free_count_++] = slot;
            }
        }
        return TxResult::Accepted;
    }
    size_t Pending() const { return outstanding_.load(std::memory_order_acquire); }
    uint32_t Submitted() const { return submitted_; }
    uint32_t Cancelled() const { return cancelled_; }
    void Clear() {
        Group group;
        for (size_t i = 0; i < Capacity && incoming_.Pop(group); ++i) Cancel(group.count);
        CancelStaged();
    }
  private:
    uint32_t Generation() const { return sink_.generation ? sink_.generation(sink_.context) : 0; }
    void Cancel(size_t count) {
        cancelled_ += uint32_t(count);
        outstanding_.fetch_sub(uint32_t(count),std::memory_order_release);
    }
    void CancelStaged() {
        for (auto* heap : {&ordinary_,&realtime_}) {
            for (size_t i = 0; i < heap->size; ++i) {
                const auto slot = heap->indices[i]; Cancel(groups_[slot].count - groups_[slot].position);
                free_[free_count_++] = slot;
            }
            heap->size = 0;
        }
        if (active_ != Capacity) {
            Cancel(groups_[active_].count - groups_[active_].position);
            free_[free_count_++] = active_; active_ = Capacity;
        }
    }
    TxResult Enqueue(const Event* events, size_t count, uint32_t deadline, bool scheduled) {
        if (!events || !count || count > MaxBundle) return TxResult::Invalid;
        Group group;
        group.deadline = deadline; group.scheduled = scheduled; group.generation = Generation();
        for (size_t i = 0; i < count; ++i) {
            const auto& e = events[i];
            if (!e.Valid()) return TxResult::Invalid;
            Event note = e;
            if ((e.type == EventType::NoteOn || e.type == EventType::NoteOff) && e.resolution == 14) {
                note.resolution = 7;
                if (e.type == EventType::NoteOn && e.value == 0) {
                    note.type = EventType::NoteOff; note.note_off_origin = NoteOffOrigin::ZeroVelocityNoteOn;
                } else if (!(e.type == EventType::NoteOff && e.note_off_origin == NoteOffOrigin::ZeroVelocityNoteOn)) {
                    if (e.type == EventType::NoteOn && e.value < 128) return TxResult::Invalid;
                    if (size_t(group.count) + 2 > MaxBundle) return TxResult::Invalid;
                    Encode(Event::Cc(e.source, e.channel, 88, uint8_t(e.value & 127)), group.messages[group.count++]);
                    note.value = uint16_t(e.value >> 7);
                }
            }
            if (group.count == MaxBundle) return TxResult::Invalid;
            const auto result = Encode(note, group.messages[group.count]);
            if (result != TxResult::Accepted) return result;
            ++group.count;
        }
        if (sink_.submit_bundle && sink_.max_bundle_messages && group.count > sink_.max_bundle_messages)
            return TxResult::Invalid;
        group.realtime = group.count == 1 && group.messages[0].bytes[0] >= 0xf8;
        auto used = outstanding_.load(std::memory_order_relaxed);
        if (used + group.count > Capacity) return TxResult::Busy;
        outstanding_.fetch_add(group.count, std::memory_order_acq_rel);
        group.order = next_order_;
        if (!incoming_.Push(group)) {
            outstanding_.fetch_sub(group.count, std::memory_order_release); return TxResult::Busy;
        }
        ++next_order_;
        return TxResult::Accepted;
    }
    bool Before(size_t a, size_t b) const {
        const auto& x = groups_[a]; const auto& y = groups_[b];
        return x.deadline == y.deadline ? int32_t(x.order - y.order) < 0
            : int32_t(x.deadline - poll_time_us_) < int32_t(y.deadline - poll_time_us_);
    }
    bool Due(const Heap& heap, uint32_t now) const {
        return heap.size && int32_t(now - groups_[heap.indices[0]].deadline) >= 0;
    }
    void Insert(Heap& heap, size_t slot) {
        size_t i = heap.size++;
        while (i && Before(slot, heap.indices[(i - 1) / 2])) {
            heap.indices[i] = heap.indices[(i - 1) / 2]; i = (i - 1) / 2;
        }
        heap.indices[i] = slot;
    }
    size_t Remove(Heap& heap) {
        const auto result = heap.indices[0]; const auto last = heap.indices[--heap.size];
        if (heap.size) {
            size_t i = 0;
            while (i * 2 + 1 < heap.size) {
                size_t child = i * 2 + 1;
                if (child + 1 < heap.size && Before(heap.indices[child + 1], heap.indices[child])) ++child;
                if (!Before(heap.indices[child], last)) break;
                heap.indices[i] = heap.indices[child]; i = child;
            }
            heap.indices[i] = last;
        }
        return result;
    }
    OutputSink sink_;
    Queue<Group, Capacity> incoming_;
    std::array<Group, Capacity> groups_{};
    std::array<size_t, Capacity> free_{};
    Heap ordinary_{}, realtime_{};
    size_t free_count_ = Capacity, active_ = Capacity;
    std::atomic<uint32_t> outstanding_{0};
    uint32_t submitted_ = 0, cancelled_ = 0, next_order_ = 0, generation_ = 0;
    uint32_t poll_time_us_ = 0;
};
}
