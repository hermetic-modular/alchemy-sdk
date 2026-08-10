/**
 * @file alchemy/midi/usb_midi.h
 * @brief USB-MIDI service over the composite device.  Target-only.
 *
 * RX: endpoint IRQ → SPSC ring → Poll() (~1 ms, from the firmware's
 * OnPoll hook) → channel filter → consumers.  TX: Send*() queue,
 * Poll() drains.  With hostlink::Host present USB is already started;
 * otherwise call Start() once after Init().
 *
 * NoteOn velocity 0 dispatches as NoteOff.  CC 64 drives sustain and
 * CC 120/123 clear on the attached tracker/allocator.  Pitch bend,
 * channel pressure and CC 74 are cached per channel (the MPE per-note
 * dimensions — an MPE consumer runs omni with the allocator and reads
 * them via Voice::channel).  RPN 0 sets the bend range until the bound
 * setting is next moved.  SysEx is delivered to OnSysEx when hooked,
 * discarded otherwise; either way the stream stays aligned.  Active
 * sensing is ignored.  Cable pull / suspend clears note state.
 */

#pragma once

#include <cstdint>

#include "alchemy/midi/midi_event.h"
#include "alchemy/midi/mono_note_tracker.h"
#include "alchemy/midi/poly_note_allocator.h"
#include "alchemy/surface/settings_control.h"
#include "alchemy/usb/usb_device.h"

namespace alchemy {
namespace midi {

class UsbMidi
{
  public:
    static constexpr uint8_t kOmni = 0xFFu;

    using EventFn = void (*)(const MidiEvent& ev, void* ctx);
    using ClockFn = void (*)(uint32_t stamp_us, void* ctx);
    using SysExFn = void (*)(const uint8_t* data, uint8_t len, bool terminal,
                             void* ctx);
    using RpnFn   = void (*)(uint8_t channel, uint16_t param, uint16_t value14,
                           bool is_nrpn, void* ctx);

    /** Registers the RX hook.  One instance per system. */
    void Init();

    /** Starts USB for firmware without hostlink::Host (first caller
     *  wins). */
    void Start(const char* product_name = nullptr,
               AlchemyUsbPeriph periph =
#if defined(ALCHEMY_BOARD_V2)
                   ALCHEMY_USB_PERIPH_HS
#else
                   ALCHEMY_USB_PERIPH_FS
#endif
    );

    UsbMidi& SetChannel(uint8_t ch_0_15)
    {
        channel_ = (ch_0_15 > 15u) ? kOmni : ch_0_15;
        return *this;
    }
    UsbMidi& SetOmni()
    {
        channel_ = kOmni;
        return *this;
    }
    uint8_t Channel() const { return channel_; }

    UsbMidi& UseTracker(MonoNoteTracker& t)
    {
        tracker_ = &t;
        return *this;
    }
    UsbMidi& UseAllocator(PolyNoteAllocator& a)
    {
        allocator_ = &a;
        return *this;
    }

    /* ── Settings recipes ───────────────────────────────────────────
     * Declare a Selector with the matching zone count, hand the handle
     * over: the recipe names + labels the field for the web editor and
     * live-binds the value (applied each Poll, persists with presets). */

    static constexpr uint8_t kChannelZones   = 17u; /* Omni + 1..16   */
    static constexpr uint8_t kBendRangeZones = 5u;  /* 0/1/2/7/12 st  */
    static constexpr uint8_t kPriorityZones  = 3u;  /* Last/Low/High  */

    static const char* const kChannelLabels[kChannelZones];
    static const char* const kBendRangeLabels[kBendRangeZones];
    static const char* const kPriorityLabels[kPriorityZones];
    static const float       kBendRangeSemis[kBendRangeZones];
    /* Convention for a firmware-owned velocity-mode Selector(3). */
    static const char* const kVelocityModeLabels[3];

    UsbMidi& UseChannelSetting(SelectorHandle h);
    UsbMidi& UseBendRangeSetting(SelectorHandle h);
    UsbMidi& UsePrioritySetting(SelectorHandle h);

    UsbMidi& OnEvent(EventFn fn, void* ctx)
    {
        event_ctx_ = ctx;
        event_fn_  = fn;
        return *this;
    }

    /** 0xF8 only, IRQ context — keep it to a couple of word stores
     *  (ClockFollower::OnPulse qualifies). */
    UsbMidi& OnClockPulse(ClockFn fn, void* ctx)
    {
        clock_ctx_ = ctx;
        clock_fn_  = fn;
        return *this;
    }

    /** Raw SysEx bytes (F0..F7 inclusive) as they arrive, ≤3 per call;
     *  terminal marks the packet carrying the end. */
    UsbMidi& OnSysEx(SysExFn fn, void* ctx)
    {
        sysex_ctx_ = ctx;
        sysex_fn_  = fn;
        return *this;
    }

    /** Decoded (N)RPN data entry, per channel. */
    UsbMidi& OnRpn(RpnFn fn, void* ctx)
    {
        rpn_ctx_ = ctx;
        rpn_fn_  = fn;
        return *this;
    }

    void Poll(uint32_t t_ms);

    /* ── TX ─────────────────────────────────────────────────────────── */

    void Send(const MidiEvent& ev);
    void SendNoteOn(uint8_t ch, uint8_t note, uint8_t vel)
    {
        Send(MidiEvent::Make(MidiMsg::NoteOn, ch, note, vel));
    }
    void SendNoteOff(uint8_t ch, uint8_t note)
    {
        Send(MidiEvent::Make(MidiMsg::NoteOff, ch, note, 0x40u));
    }
    void SendControlChange(uint8_t ch, uint8_t cc, uint8_t val)
    {
        Send(MidiEvent::Make(MidiMsg::ControlChange, ch, cc, val));
    }
    void SendProgramChange(uint8_t ch, uint8_t program)
    {
        Send(MidiEvent::Make(MidiMsg::ProgramChange, ch, program));
    }
    void SendChannelPressure(uint8_t ch, uint8_t value)
    {
        Send(MidiEvent::Make(MidiMsg::ChannelPressure, ch, value));
    }
    void SendPitchBend(uint8_t ch, uint16_t bend14)
    {
        Send(MidiEvent::Make(MidiMsg::PitchBend, ch,
                             static_cast<uint8_t>(bend14 & 0x7Fu),
                             static_cast<uint8_t>((bend14 >> 7) & 0x7Fu)));
    }
    /** Clock / Start / Continue / Stop / SystemReset. */
    void SendRealtime(MidiMsg m)
    {
        MidiEvent ev;
        ev.status = static_cast<uint8_t>(m);
        Send(ev);
    }
    /** Queues F0 + payload + F7.  False (and counted) if the queue
     *  can't take the whole message now. */
    bool SendSysEx(const uint8_t* payload, uint16_t len);

    /* ── State ──────────────────────────────────────────────────────── */

    /** Last accepted pitch bend; 0x2000 center. */
    uint16_t PitchBend14() const { return bend14_; }
    uint16_t PitchBend14(uint8_t ch) const { return bend14_ch_[ch & 0x0Fu]; }
    float    BendSemis(float range_semis) const
    {
        return BendVal(bend14_, range_semis);
    }
    /** Semitones at the effective bend range (bound setting, or RPN 0
     *  when a host has sent one; ±2 default). */
    float BendSemis() const { return BendVal(bend14_, bend_range_semis_); }
    float BendSemis(uint8_t ch) const
    {
        return BendVal(bend14_ch_[ch & 0x0Fu], bend_range_semis_);
    }
    float   BendRangeSemis() const { return bend_range_semis_; }
    uint8_t ChannelPressure(uint8_t ch) const
    {
        return pressure_[ch & 0x0Fu];
    }
    /** CC 74 (MPE timbre / brightness), per channel. */
    uint8_t Cc74(uint8_t ch) const { return cc74_[ch & 0x0Fu]; }

    bool HostConnected() const { return alchemy_usb_configured(); }

    uint32_t RxDropped() const { return rx_dropped_; }
    uint32_t TxDropped() const { return tx_dropped_; }
    uint32_t ClockCount() const { return clock_count_; }

  private:
    static constexpr uint16_t kRxRing = 256u; /* packets (pow-2) */
    static constexpr uint16_t kTxRing = 256u; /* packets (pow-2) */

    static void  RxTrampoline(const uint8_t pkt[4], void* ctx);
    void         OnRxPacket(const uint8_t pkt[4]);
    void         Dispatch(const MidiEvent& ev);
    void         HandleCc(uint8_t ch, uint8_t cc, uint8_t val);
    void         HandleSysExPacket(const uint8_t pkt[4]);
    void         PushPacket(const uint8_t pkt[4]);
    uint16_t     TxFree() const;
    void         DrainTx();
    void         ResetChannelState();
    static float BendVal(uint16_t b14, float range)
    {
        return (static_cast<float>(b14) - 8192.0f) * (1.0f / 8192.0f) * range;
    }

    static UsbMidi* s_instance;

    uint8_t           rx_[kRxRing][4];
    volatile uint16_t rx_w_ = 0u; /* IRQ writes  */
    uint16_t          rx_r_ = 0u; /* Poll reads  */

    uint8_t  tx_[kTxRing][4];
    uint16_t tx_w_ = 0u;
    uint16_t tx_r_ = 0u;

    MonoNoteTracker*   tracker_   = nullptr;
    PolyNoteAllocator* allocator_ = nullptr;

    EventFn event_fn_  = nullptr;
    void*   event_ctx_ = nullptr;
    ClockFn clock_fn_  = nullptr;
    void*   clock_ctx_ = nullptr;
    SysExFn sysex_fn_  = nullptr;
    void*   sysex_ctx_ = nullptr;
    RpnFn   rpn_fn_    = nullptr;
    void*   rpn_ctx_   = nullptr;

    uint8_t  channel_    = kOmni;
    bool     was_online_ = false;
    uint32_t rx_dropped_ = 0u;
    uint32_t tx_dropped_ = 0u;
    volatile uint32_t clock_count_ = 0u;

    uint16_t bend14_       = 0x2000u;
    uint16_t bend14_ch_[16];
    uint8_t  pressure_[16] = {};
    uint8_t  cc74_[16]     = {};

    /* (N)RPN state machine, per channel.  mode: 0 none / 1 RPN / 2 NRPN. */
    uint8_t rpn_mode_[16] = {};
    uint8_t param_msb_[16] = {};
    uint8_t param_lsb_[16] = {};
    uint8_t data_msb_[16]  = {};

    /* Live-bound setting zones (Settings-owned storage). */
    const uint8_t* channel_zone_     = nullptr;
    const uint8_t* bend_zone_        = nullptr;
    const uint8_t* priority_zone_    = nullptr;
    uint8_t        last_bend_zone_   = 0xFFu;
    float          bend_range_semis_ = 2.0f;
};

} // namespace midi
} // namespace alchemy
