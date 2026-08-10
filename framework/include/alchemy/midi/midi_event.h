/**
 * @file alchemy/midi/midi_event.h
 * @brief MIDI event type + USB-MIDI 1.0 event-packet codec.  Pure,
 *        host-tested; USB framing delimits messages per packet, so no
 *        running-status handling exists here.
 */

#pragma once

#include <cstdint>

namespace alchemy {
namespace midi {

enum class MidiMsg : uint8_t
{
    NoteOff         = 0x80,
    NoteOn          = 0x90,
    PolyPressure    = 0xA0,
    ControlChange   = 0xB0,
    ProgramChange   = 0xC0,
    ChannelPressure = 0xD0,
    PitchBend       = 0xE0,
    SongPosition    = 0xF2,
    SongSelect      = 0xF3,
    TuneRequest     = 0xF6,
    Clock           = 0xF8,
    Start           = 0xFA,
    Continue        = 0xFB,
    Stop            = 0xFC,
    ActiveSensing   = 0xFE,
    SystemReset     = 0xFF,
};

struct MidiEvent
{
    uint8_t status = 0u;
    uint8_t d1     = 0u;
    uint8_t d2     = 0u;

    bool IsChannelVoice() const
    {
        return status >= 0x80u && status < 0xF0u;
    }
    uint8_t Channel() const { return status & 0x0Fu; }
    MidiMsg Type() const
    {
        return static_cast<MidiMsg>(IsChannelVoice() ? (status & 0xF0u)
                                                     : status);
    }
    /** 14-bit pitch bend, 0x2000 = center. */
    uint16_t Bend14() const
    {
        return static_cast<uint16_t>(d1 | (static_cast<uint16_t>(d2) << 7));
    }

    static MidiEvent Make(MidiMsg m, uint8_t ch, uint8_t d1, uint8_t d2 = 0u)
    {
        MidiEvent e;
        const uint8_t s = static_cast<uint8_t>(m);
        e.status = (s < 0xF0u) ? static_cast<uint8_t>(s | (ch & 0x0Fu)) : s;
        e.d1     = d1 & 0x7Fu;
        e.d2     = d2 & 0x7Fu;
        return e;
    }
};

/** Decodes one event packet.  False for packets with no event
 *  semantics here: reserved CINs and SysEx spans (discarded). */
inline bool EventFromUsbPacket(const uint8_t pkt[4], MidiEvent& out)
{
    const uint8_t cin = pkt[0] & 0x0Fu;
    switch (cin)
    {
        case 0x2u: /* 2-byte system common (F1, F3) */
        case 0xCu: /* program change */
        case 0xDu: /* channel pressure */
            out.status = pkt[1];
            out.d1     = pkt[2];
            out.d2     = 0u;
            return true;

        case 0x3u: /* 3-byte system common (F2) */
        case 0x8u:
        case 0x9u:
        case 0xAu:
        case 0xBu:
        case 0xEu:
            out.status = pkt[1];
            out.d1     = pkt[2];
            out.d2     = pkt[3];
            return true;

        case 0x5u: /* 1-byte system common — or 1-byte SysEx end */
            if (pkt[1] == 0xF6u)
            {
                out = {0xF6u, 0u, 0u};
                return true;
            }
            return false;

        case 0xFu: /* single byte (realtime) */
            if (pkt[1] >= 0xF8u)
            {
                out = {pkt[1], 0u, 0u};
                return true;
            }
            return false;

        default: /* 0x0, 0x1, 0x4, 0x6, 0x7 */
            return false;
    }
}

/** Encodes an event as a packet; false if it has no packet form. */
inline bool EventToUsbPacket(const MidiEvent& ev, uint8_t cable,
                             uint8_t pkt[4])
{
    uint8_t cin;
    uint8_t n = 3u;

    if (ev.status >= 0xF8u)
    {
        cin = 0xFu;
        n   = 1u;
    }
    else if (ev.status == 0xF2u)
    {
        cin = 0x3u;
    }
    else if (ev.status == 0xF1u || ev.status == 0xF3u)
    {
        cin = 0x2u;
        n   = 2u;
    }
    else if (ev.status == 0xF6u)
    {
        cin = 0x5u;
        n   = 1u;
    }
    else if (ev.status >= 0x80u && ev.status < 0xF0u)
    {
        cin = ev.status >> 4;
        if (cin == 0xCu || cin == 0xDu)
        {
            n = 2u;
        }
    }
    else
    {
        return false;
    }

    pkt[0] = static_cast<uint8_t>((cable << 4) | cin);
    pkt[1] = ev.status;
    pkt[2] = (n >= 2u) ? ev.d1 : 0u;
    pkt[3] = (n >= 3u) ? ev.d2 : 0u;
    return true;
}

} // namespace midi
} // namespace alchemy
