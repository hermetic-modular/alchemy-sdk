/**
 * @file surface/button_bank.cpp
 * @brief alchemy::ButtonBank implementation.
 */

#include "alchemy/surface/button_bank.h"

#include <cstdio>
#include <cstring>

#include "alchemy/hw/i_button.h"
#include "alchemy/led/panel.h"
#include "alchemy/surface/control_loop.h"
#include "alchemy/surface/knob_storage.h"
#include "alchemy/surface/page.h"

namespace alchemy {

/* Effective field id: the declared Ident(), or the positional "b<hw>"
 * fallback (mirrors the pager's "p<page>.<pot>" default). */
static const char* EffectiveId(const VirtualButton& b, char* buf, size_t cap)
{
    if (b.Ident()) return b.Ident();
    std::snprintf(buf, cap, "b%u", b.HwIndex());
    return buf;
}

/* ── Page source ────────────────────────────────────────────────────── */

uint8_t ButtonBank::NumSourcePages() const
{
    if (num_pages_ > 0u) return num_pages_;
    return loop_ ? loop_->NumAttachedPages() : 0u;
}

const Page* ButtonBank::SourcePage(uint8_t i) const
{
    if (num_pages_ > 0u) return (i < num_pages_) ? pages_[i] : nullptr;
    return loop_ ? loop_->AttachedPage(i) : nullptr;
}

/* ── Roster freeze ──────────────────────────────────────────────────── */

void ButtonBank::EnrollState(VirtualButton& b, int16_t page) const
{
    for (uint8_t i = 0; i < num_cells_; i++)
    {
        if (cells_[i].btn == &b)
        {
            if (cells_[i].page != page) cells_[i].page = -1;
            return;
        }
    }
    if (num_cells_ >= kMaxCells) { ok_ = false; return; }

    Cell& c = cells_[num_cells_];
    c.btn   = &b;
    c.zone  = b.DefaultZone();
    c.page  = page;
    c.dirty = false;

    /* Declaration validation — any failure latches, and the descriptor
     * build then fails instead of shipping a wrong layout. */
    if (b.Zones() < 2u) ok_ = false;
    if (b.DefaultZone() >= b.Zones()) ok_ = false;
    if (b.ZoneLabels() && b.NumZoneLabels() != b.Zones()) ok_ = false;
    if (b.ZoneColors() && b.NumZoneColors() != 0u
        && b.NumZoneColors() != b.Zones())
        ok_ = false;
    if (b.HasHw() && num_hw_ > 0u && b.HwIndex() >= num_hw_) ok_ = false;
    if (!b.Ident() && !b.HasHw()) ok_ = false;   /* no derivable id */
    char id_a[8], id_b[8];
    const char* bid = EffectiveId(b, id_b, sizeof id_b);
    for (uint8_t i = 0; i < num_cells_; i++)
        if (std::strcmp(EffectiveId(*cells_[i].btn, id_a, sizeof id_a), bid)
            == 0)
            ok_ = false;

    const bool* pressed = (b.HasHw() && b.HwIndex() < kMaxHw)
                              ? &pressed_now_[b.HwIndex()]
                              : nullptr;
    b.Attach(&c.zone, pressed);
    num_cells_++;
}

void ButtonBank::EnrollModal(VirtualButton& b, int16_t page) const
{
    for (uint8_t i = 0; i < num_modal_; i++)
    {
        if (modal_[i].btn == &b)
        {
            if (modal_[i].page != page) modal_[i].page = -1;
            return;
        }
    }
    if (num_modal_ >= kMaxModal) { ok_ = false; return; }
    if (b.HasHw() && num_hw_ > 0u && b.HwIndex() >= num_hw_) ok_ = false;

    modal_[num_modal_].btn  = &b;
    modal_[num_modal_].page = page;

    const bool* pressed = (b.HasHw() && b.HwIndex() < kMaxHw)
                              ? &pressed_now_[b.HwIndex()]
                              : nullptr;
    b.Attach(nullptr, pressed);
    num_modal_++;
}

void ButtonBank::FreezeIfNeeded() const
{
    if (frozen_) return;
    frozen_ = true;

    for (uint8_t i = 0; i < NumSourcePages(); i++)
    {
        const Page* p = SourcePage(i);
        if (!p) continue;
        for (uint8_t j = 0; j < p->NumButtons(); j++)
        {
            VirtualButton* b = p->ButtonAt(j);
            if (!b) continue;
            if (b->HasState())   EnrollState(*b, p->Index());
            else if (b->HasHw()) EnrollModal(*b, p->Index());
        }
    }
    for (uint8_t i = 0; i < num_globals_; i++)
    {
        VirtualButton* b = globals_[i];
        if (b->HasState())   EnrollState(*b, -1);
        else if (b->HasHw()) EnrollModal(*b, -1);
    }
}

int ButtonBank::FindCell(const VirtualButton& b) const
{
    for (uint8_t i = 0; i < num_cells_; i++)
        if (cells_[i].btn == &b) return i;
    return -1;
}

/* ── Gesture dispatch ───────────────────────────────────────────────────
 * Dispatch scans live page membership (the press-latched page's buttons,
 * then globals) — a button moved between pages at runtime dispatches in
 * its new home; only its persisted cell is fixed at freeze. */

void ButtonBank::FireGesture(VirtualButton& b,
                             const VirtualButton::Gesture& g, uint8_t hw)
{
    const bool claims =
        storage_ && hw_btns_ && hw < num_hw_
        && hw_btns_[hw] == storage_->PageButton();

    if (g.fn)
    {
        g.fn(g.fn_ctx);
        if (claims) storage_->ConsumeButton();
        return;
    }
    if (!b.HasState()) return;   /* action gesture on a momentary: inert */

    const int idx = FindCell(b);
    if (idx < 0) { ok_ = false; return; }   /* declared after freeze */

    Cell&         c     = cells_[idx];
    const uint8_t zones = b.Zones();
    uint8_t       next  = c.zone;
    switch (g.action)
    {
        case VirtualButton::Action::Cycle:
            next = static_cast<uint8_t>((c.zone + 1u) % zones);
            break;
        case VirtualButton::Action::Toggle:
            next = (c.zone == 0u) ? 1u : 0u;
            break;
        case VirtualButton::Action::Set:
            next = (g.set_zone < zones) ? g.set_zone
                                        : static_cast<uint8_t>(zones - 1u);
            break;
    }
    if (claims) storage_->ConsumeButton();
    if (next != c.zone)
    {
        c.zone = next;
        b.NotifyChange(next);
    }
}

void ButtonBank::DispatchHolds(uint8_t hw, uint32_t t_ms)
{
    HwState& hs = hw_state_[hw];
    uint8_t  scan = 0;

    const uint8_t n = NumSourcePages();
    for (uint8_t i = 0; i < n; i++)
    {
        const Page* p = SourcePage(i);
        if (!p || p->Index() != hs.press_page) continue;
        for (uint8_t j = 0; j < p->NumButtons(); j++, scan++)
        {
            VirtualButton* b = p->ButtonAt(j);
            if (!b || b->HwIndex() != hw) continue;
            const VirtualButton::Gesture& g = b->HoldGesture();
            if (!g.used || (hs.hold_fired & (1u << (scan & 7u)))) continue;
            if (t_ms - hs.press_t >= g.hold_ms)
            {
                hs.hold_fired |= static_cast<uint8_t>(1u << (scan & 7u));
                hs.hold_any = true;
                FireGesture(*b, g, hw);
            }
        }
    }
    for (uint8_t i = 0; i < num_globals_; i++, scan++)
    {
        VirtualButton* b = globals_[i];
        if (!b || b->HwIndex() != hw) continue;
        const VirtualButton::Gesture& g = b->HoldGesture();
        if (!g.used || (hs.hold_fired & (1u << (scan & 7u)))) continue;
        if (t_ms - hs.press_t >= g.hold_ms)
        {
            hs.hold_fired |= static_cast<uint8_t>(1u << (scan & 7u));
            hs.hold_any = true;
            FireGesture(*b, g, hw);
        }
    }
}

void ButtonBank::DispatchTaps(uint8_t hw)
{
    const HwState& hs = hw_state_[hw];

    auto fire_tap = [&](VirtualButton& b)
    {
        if (b.HwIndex() != hw) return;
        VirtualButton::Gesture g = b.TapGesture();
        if (!g.used)
        {
            /* Derived default: a stateful button with no declared
             * gestures tap-cycles its zones. */
            if (!b.HasState() || b.HoldGesture().used) return;
            g.used   = true;
            g.action = VirtualButton::Action::Cycle;
        }
        FireGesture(b, g, hw);
    };

    const uint8_t n = NumSourcePages();
    for (uint8_t i = 0; i < n; i++)
    {
        const Page* p = SourcePage(i);
        if (!p || p->Index() != hs.press_page) continue;
        for (uint8_t j = 0; j < p->NumButtons(); j++)
            if (VirtualButton* b = p->ButtonAt(j)) fire_tap(*b);
    }
    for (uint8_t i = 0; i < num_globals_; i++)
        if (globals_[i]) fire_tap(*globals_[i]);
}

void ButtonBank::PollButtons(uint32_t t_ms, bool gated, uint8_t active_page)
{
    FreezeIfNeeded();

    for (uint8_t hw = 0; hw < num_hw_ && hw < kMaxHw; hw++)
    {
        const bool pressed = hw_btns_[hw]->Pressed();
        pressed_now_[hw]   = pressed;

        HwState& hs = hw_state_[hw];
        const bool rising  = pressed && !hs.prev;
        const bool falling = !pressed && hs.prev;
        hs.prev = pressed;

        if (gated)
        {
            /* Poison any in-flight press: a hold must not fire from a
             * stale press_t when the gate lifts, and the eventual
             * release is not a tap.  A fresh press resets both. */
            hs.hold_any   = true;
            hs.hold_fired = 0xFFu;
            continue;
        }

        if (rising)
        {
            hs.press_t    = t_ms;
            hs.press_page = active_page;   /* latch: gestures resolve here */
            hs.hold_any   = false;
            hs.hold_fired = 0;
        }
        if (pressed) DispatchHolds(hw, t_ms);
        if (falling && !hs.hold_any) DispatchTaps(hw);
    }
}

/* ── Deferred change flush ──────────────────────────────────────────── */

void ButtonBank::Update(uint32_t /*t_ms*/)
{
    for (uint8_t i = 0; i < num_cells_; i++)
    {
        if (!cells_[i].dirty) continue;
        cells_[i].dirty = false;
        cells_[i].btn->NotifyChange(cells_[i].zone);
    }
}

/* ── Render ─────────────────────────────────────────────────────────── */

void ButtonBank::Render(LedPanel& panel, uint8_t active_page,
                        uint32_t /*t_ms*/) const
{
    FreezeIfNeeded();

    auto paint = [&](const VirtualButton& b)
    {
        if (!b.HasState() || !b.HasHw() || !b.ZoneColors()) return;
        const int idx = FindCell(b);
        if (idx < 0) return;
        panel.SetButtonPair(b.HwIndex(), b.ZoneColors()[cells_[idx].zone]);
    };

    const uint8_t n = NumSourcePages();
    for (uint8_t i = 0; i < n; i++)
    {
        const Page* p = SourcePage(i);
        if (!p || p->Index() != active_page) continue;
        for (uint8_t j = 0; j < p->NumButtons(); j++)
            if (const VirtualButton* b = p->ButtonAt(j)) paint(*b);
    }
    for (uint8_t i = 0; i < num_globals_; i++)
        if (globals_[i]) paint(*globals_[i]);
}

/* ── State access ───────────────────────────────────────────────────── */

uint8_t ButtonBank::ZoneOf(const VirtualButton& b) const
{
    FreezeIfNeeded();
    const int idx = FindCell(b);
    return (idx >= 0) ? cells_[idx].zone : b.DefaultZone();
}

void ButtonBank::SetZone(const VirtualButton& b, uint8_t zone)
{
    FreezeIfNeeded();
    const int idx = FindCell(b);
    if (idx < 0 || zone >= b.Zones()) return;
    if (cells_[idx].zone == zone) return;
    cells_[idx].zone  = zone;
    cells_[idx].dirty = true;
}

/* ── Serializable ───────────────────────────────────────────────────── */

size_t ButtonBank::SerializedSize() const
{
    FreezeIfNeeded();
    return num_cells_;
}

void ButtonBank::Serialize(uint8_t* out) const
{
    FreezeIfNeeded();
    for (uint8_t i = 0; i < num_cells_; i++) out[i] = cells_[i].zone;
}

bool ButtonBank::Deserialize(const uint8_t* in)
{
    FreezeIfNeeded();
    for (uint8_t i = 0; i < num_cells_; i++)
    {
        uint8_t v = in[i];
        if (v >= cells_[i].btn->Zones()) v = cells_[i].btn->DefaultZone();
        if (v != cells_[i].zone)
        {
            cells_[i].zone  = v;
            cells_[i].dirty = true;   /* notified from the next Update() */
        }
    }
    return true;
}

uint32_t ButtonBank::SchemaHash() const
{
    FreezeIfNeeded();

    /* FNV-1a over each cell's (effective ident, zones), order-sensitive:
     * any roster reshape invalidates saved slots instead of misreading. */
    uint32_t h = 2166136261u;
    auto fold  = [&h](uint8_t byte) { h = (h ^ byte) * 16777619u; };
    fold('B'); fold('T'); fold('N'); fold('S');
    for (uint8_t i = 0; i < num_cells_; i++)
    {
        const VirtualButton& b = *cells_[i].btn;
        char id_buf[8];
        for (const char* s = EffectiveId(b, id_buf, sizeof id_buf); *s; s++)
            fold(static_cast<uint8_t>(*s));
        fold(0u);
        fold(b.Zones());
    }
    return h;
}

/* ── Descriptor introspection ───────────────────────────────────────── */

uint8_t ButtonBank::NumCells() const
{
    FreezeIfNeeded();
    return num_cells_;
}

const VirtualButton* ButtonBank::CellAt(uint8_t i) const
{
    FreezeIfNeeded();
    return (i < num_cells_) ? cells_[i].btn : nullptr;
}

int16_t ButtonBank::CellPage(uint8_t i) const
{
    FreezeIfNeeded();
    return (i < num_cells_) ? cells_[i].page : -1;
}

uint8_t ButtonBank::NumModal() const
{
    FreezeIfNeeded();
    return num_modal_;
}

const VirtualButton* ButtonBank::ModalAt(uint8_t i) const
{
    FreezeIfNeeded();
    return (i < num_modal_) ? modal_[i].btn : nullptr;
}

int16_t ButtonBank::ModalPage(uint8_t i) const
{
    FreezeIfNeeded();
    return (i < num_modal_) ? modal_[i].page : -1;
}

} // namespace alchemy
