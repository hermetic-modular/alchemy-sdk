#include "alchemy/midi/usb_midi.h"

#include "daisy_seed.h"

namespace alchemy {
namespace midi {

UsbMidi* UsbMidi::s_instance = nullptr;

const char* const UsbMidi::kChannelLabels[UsbMidi::kChannelZones] = {
    "Omni", "1", "2", "3", "4", "5", "6", "7", "8",
    "9", "10", "11", "12", "13", "14", "15", "16",
};
const char* const UsbMidi::kBendRangeLabels[UsbMidi::kBendRangeZones] = {
    "Off", "±1", "±2", "±7", "±12",
};
const float UsbMidi::kBendRangeSemis[UsbMidi::kBendRangeZones] = {
    0.0f, 1.0f, 2.0f, 7.0f, 12.0f,
};
const char* const UsbMidi::kPriorityLabels[UsbMidi::kPriorityZones] = {
    "Last", "Low", "High",
};
const char* const UsbMidi::kVelocityModeLabels[3] = {
    "Off", "Accent", "Level",
};

UsbMidi& UsbMidi::UseChannelSetting(SelectorHandle h)
{
    h.Name("MIDI Channel").Labels(kChannelLabels, kChannelZones);
    channel_zone_ = h.IndexPtr();
    return *this;
}

UsbMidi& UsbMidi::UseBendRangeSetting(SelectorHandle h)
{
    h.Name("Bend Range").Labels(kBendRangeLabels, kBendRangeZones);
    bend_zone_ = h.IndexPtr();
    return *this;
}

UsbMidi& UsbMidi::UsePrioritySetting(SelectorHandle h)
{
    h.Name("Note Priority").Labels(kPriorityLabels, kPriorityZones);
    priority_zone_ = h.IndexPtr();
    return *this;
}

void UsbMidi::Init()
{
    s_instance = this;
    for (uint8_t i = 0u; i < 16u; i++) bend14_ch_[i] = 0x2000u;
    alchemy_usb_midi_set_rx(&UsbMidi::RxTrampoline, this);
}

void UsbMidi::Start(const char* product_name, AlchemyUsbPeriph periph)
{
    alchemy_usb_start(periph, product_name);
}

void UsbMidi::RxTrampoline(const uint8_t pkt[4], void* ctx)
{
    static_cast<UsbMidi*>(ctx)->OnRxPacket(pkt);
}

void UsbMidi::OnRxPacket(const uint8_t pkt[4])
{
    /* IRQ context: single producer. */
    const uint8_t cin = pkt[0] & 0x0Fu;

    if (cin == 0x0Fu && pkt[1] >= 0xF8u)
    {
        if (pkt[1] == 0xF8u)
        {
            clock_count_ = clock_count_ + 1u;
            if (clock_fn_ != nullptr)
            {
                clock_fn_(daisy::System::GetUs(), clock_ctx_);
            }
            return;
        }
        if (pkt[1] == 0xFEu)
        {
            return;
        }
    }

    const uint16_t next = static_cast<uint16_t>((rx_w_ + 1u) & (kRxRing - 1u));
    if (next == rx_r_)
    {
        rx_dropped_++;
        return;
    }
    rx_[rx_w_][0] = pkt[0];
    rx_[rx_w_][1] = pkt[1];
    rx_[rx_w_][2] = pkt[2];
    rx_[rx_w_][3] = pkt[3];
    rx_w_         = next;
}

void UsbMidi::HandleCc(uint8_t ch, uint8_t cc, uint8_t val)
{
    switch (cc)
    {
        case 64u:
            if (tracker_ != nullptr) tracker_->SetSustain(val >= 64u);
            if (allocator_ != nullptr) allocator_->SetSustain(val >= 64u);
            break;

        case 74u: cc74_[ch] = val; break;

        case 98u:
            rpn_mode_[ch]  = 2u;
            param_lsb_[ch] = val;
            break;
        case 99u:
            rpn_mode_[ch]  = 2u;
            param_msb_[ch] = val;
            break;
        case 100u:
            param_lsb_[ch] = val;
            rpn_mode_[ch] =
                (param_msb_[ch] == 0x7Fu && val == 0x7Fu) ? 0u : 1u;
            break;
        case 101u:
            param_msb_[ch] = val;
            rpn_mode_[ch] =
                (val == 0x7Fu && param_lsb_[ch] == 0x7Fu) ? 0u : 1u;
            break;

        case 6u:
        case 38u:
        {
            if (rpn_mode_[ch] == 0u) break;
            if (cc == 6u) data_msb_[ch] = val;
            const uint8_t  lsb = (cc == 38u) ? val : 0u;
            const uint16_t param =
                static_cast<uint16_t>((param_msb_[ch] << 7) | param_lsb_[ch]);
            const uint16_t value14 =
                static_cast<uint16_t>((data_msb_[ch] << 7) | lsb);
            if (rpn_mode_[ch] == 1u && param == 0u)
            {
                bend_range_semis_ = static_cast<float>(data_msb_[ch])
                                    + static_cast<float>(lsb) * 0.01f;
            }
            if (rpn_fn_ != nullptr)
            {
                rpn_fn_(ch, param, value14, rpn_mode_[ch] == 2u, rpn_ctx_);
            }
            break;
        }

        case 120u:
        case 123u:
            if (tracker_ != nullptr) tracker_->Clear();
            if (allocator_ != nullptr) allocator_->Clear();
            break;

        default: break;
    }
}

void UsbMidi::Dispatch(const MidiEvent& ev)
{
    const uint8_t ch = ev.Channel();

    if (ev.IsChannelVoice())
    {
        switch (ev.Type())
        {
            case MidiMsg::NoteOn:
                if (tracker_ != nullptr) tracker_->NoteOn(ev.d1, ev.d2);
                if (allocator_ != nullptr)
                    allocator_->NoteOn(ch, ev.d1, ev.d2);
                break;
            case MidiMsg::NoteOff:
                if (tracker_ != nullptr) tracker_->NoteOff(ev.d1);
                if (allocator_ != nullptr) allocator_->NoteOff(ch, ev.d1);
                break;
            case MidiMsg::ControlChange: HandleCc(ch, ev.d1, ev.d2); break;
            case MidiMsg::PitchBend:
                bend14_        = ev.Bend14();
                bend14_ch_[ch] = ev.Bend14();
                break;
            case MidiMsg::ChannelPressure: pressure_[ch] = ev.d1; break;
            default: break;
        }
    }
    if (event_fn_ != nullptr)
    {
        event_fn_(ev, event_ctx_);
    }
}

void UsbMidi::HandleSysExPacket(const uint8_t pkt[4])
{
    if (sysex_fn_ == nullptr)
    {
        return;
    }
    const uint8_t cin = pkt[0] & 0x0Fu;
    uint8_t       len;
    bool          terminal;
    switch (cin)
    {
        case 0x4u: len = 3u; terminal = false; break;
        case 0x5u: len = 1u; terminal = true; break;
        case 0x6u: len = 2u; terminal = true; break;
        case 0x7u: len = 3u; terminal = true; break;
        default: return;
    }
    sysex_fn_(&pkt[1], len, terminal, sysex_ctx_);
}

void UsbMidi::ResetChannelState()
{
    bend14_ = 0x2000u;
    for (uint8_t i = 0u; i < 16u; i++)
    {
        bend14_ch_[i] = 0x2000u;
        pressure_[i]  = 0u;
        cc74_[i]      = 0u;
        rpn_mode_[i]  = 0u;
    }
}

void UsbMidi::Poll(uint32_t t_ms)
{
    (void)t_ms;

    if (channel_zone_ != nullptr)
    {
        const uint8_t z = *channel_zone_;
        channel_ = (z == 0u) ? kOmni : static_cast<uint8_t>((z - 1u) & 0x0Fu);
    }
    /* Applied on change only, so a host-sent RPN 0 holds until the user
     * next moves the setting. */
    if (bend_zone_ != nullptr && *bend_zone_ != last_bend_zone_)
    {
        last_bend_zone_ = *bend_zone_;
        const uint8_t z = (last_bend_zone_ < kBendRangeZones)
                              ? last_bend_zone_
                              : static_cast<uint8_t>(kBendRangeZones - 1u);
        bend_range_semis_ = kBendRangeSemis[z];
    }
    if (priority_zone_ != nullptr && tracker_ != nullptr)
    {
        const uint8_t z =
            (*priority_zone_ < kPriorityZones) ? *priority_zone_ : 0u;
        tracker_->SetPriority(static_cast<MonoNoteTracker::Priority>(z));
    }

    const bool online = alchemy_usb_configured();
    if (was_online_ && !online)
    {
        if (tracker_ != nullptr) tracker_->Clear();
        if (allocator_ != nullptr) allocator_->Clear();
        ResetChannelState();
    }
    was_online_ = online;

    const uint16_t w = rx_w_;
    while (rx_r_ != w)
    {
        MidiEvent ev;
        if (EventFromUsbPacket(rx_[rx_r_], ev))
        {
            if (!ev.IsChannelVoice() || channel_ == kOmni
                || ev.Channel() == channel_)
            {
                if (ev.Type() == MidiMsg::NoteOn && ev.d2 == 0u)
                {
                    ev.status = static_cast<uint8_t>(
                        static_cast<uint8_t>(MidiMsg::NoteOff) | ev.Channel());
                    ev.d2 = 0x40u;
                }
                Dispatch(ev);
            }
        }
        else
        {
            HandleSysExPacket(rx_[rx_r_]);
        }
        rx_r_ = static_cast<uint16_t>((rx_r_ + 1u) & (kRxRing - 1u));
    }

    DrainTx();
}

uint16_t UsbMidi::TxFree() const
{
    const uint16_t used = static_cast<uint16_t>((tx_w_ - tx_r_)
                                                & (kTxRing - 1u));
    return static_cast<uint16_t>(kTxRing - 1u - used);
}

void UsbMidi::PushPacket(const uint8_t pkt[4])
{
    const uint16_t next = static_cast<uint16_t>((tx_w_ + 1u) & (kTxRing - 1u));
    if (next == tx_r_)
    {
        tx_dropped_++;
        return;
    }
    tx_[tx_w_][0] = pkt[0];
    tx_[tx_w_][1] = pkt[1];
    tx_[tx_w_][2] = pkt[2];
    tx_[tx_w_][3] = pkt[3];
    tx_w_         = next;
}

void UsbMidi::Send(const MidiEvent& ev)
{
    uint8_t pkt[4];
    if (!EventToUsbPacket(ev, 0u, pkt))
    {
        tx_dropped_++;
        return;
    }
    PushPacket(pkt);
}

bool UsbMidi::SendSysEx(const uint8_t* payload, uint16_t len)
{
    const uint32_t total   = static_cast<uint32_t>(len) + 2u; /* F0 .. F7 */
    const uint32_t packets = (total + 2u) / 3u;
    if (packets > TxFree())
    {
        tx_dropped_++;
        return false;
    }

    uint8_t buf[3];
    uint8_t n = 0u;
    for (uint32_t i = 0u; i < total; i++)
    {
        const uint8_t b = (i == 0u) ? 0xF0u
                          : (i == total - 1u) ? 0xF7u
                                              : payload[i - 1u];
        buf[n++] = b;
        const bool last = (i == total - 1u);
        if (n == 3u || last)
        {
            uint8_t pkt[4] = {0u, 0u, 0u, 0u};
            pkt[0] = last ? (n == 1u ? 0x05u : (n == 2u ? 0x06u : 0x07u))
                          : 0x04u;
            for (uint8_t k = 0u; k < n; k++) pkt[1u + k] = buf[k];
            PushPacket(pkt);
            n = 0u;
        }
    }
    return true;
}

void UsbMidi::DrainTx()
{
    if (tx_r_ == tx_w_ || !alchemy_usb_midi_tx_ready())
    {
        return;
    }

    uint8_t  burst[ALCHEMY_MIDI_EP_PACKET_SIZE];
    uint16_t n = 0u;
    uint16_t r = tx_r_;

    while (r != tx_w_ && (n + 4u) <= sizeof burst)
    {
        burst[n]      = tx_[r][0];
        burst[n + 1u] = tx_[r][1];
        burst[n + 2u] = tx_[r][2];
        burst[n + 3u] = tx_[r][3];
        n             = static_cast<uint16_t>(n + 4u);
        r             = static_cast<uint16_t>((r + 1u) & (kTxRing - 1u));
    }

    if (alchemy_usb_midi_write(burst, n) == n)
    {
        tx_r_ = r;
    }
}

} // namespace midi
} // namespace alchemy
