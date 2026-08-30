/**
 * @file surface/pager.cpp
 * @brief alchemy::Pager implementation.
 */

#include "alchemy/surface/pager.h"
#include <cstring>

namespace alchemy {

/* Schema tag — bump intentionally if the on-flash layout for Pager changes.
 * Navigation bindings are configuration, not state: 'PAG0' stands. */
static constexpr uint32_t kPagerSchemaTag = 0x50414730u; /* 'PAG0' */


Pager::Pager(uint8_t num_pages, uint8_t num_pots)
    : num_pages_((num_pages == 0u) ? 1u
                 : (num_pages > kMaxPages ? kMaxPages : num_pages)),
      num_pots_((num_pots == 0u) ? 1u
                : (num_pots > kMaxPots ? kMaxPots : num_pots))
{
    for (uint8_t pg = 0; pg < kMaxPages; pg++)
        for (uint8_t p = 0; p < kMaxPots; p++)
            states_[pg][p] = PotState{0.5f, true, 0};
}

Pager::Pager(IButton& b1, uint8_t num_pages, uint8_t num_pots)
    : Pager(num_pages, num_pots)
{
    Cycle(b1);   /* the classic gesture: one button, every page, in order */
}

/* ── Navigation declaration ────────────────────────────────────────────── */

Pager::NavBinding* Pager::AddBinding(NavKind kind, IButton& b)
{
    last_binding_ = -1;
    if (num_bindings_ >= kMaxBindings) return nullptr;
    for (uint8_t i = 0; i < num_bindings_; i++)
        if (bindings_[i].btn == &b) return nullptr;   /* one binding per button */

    NavBinding& n = bindings_[num_bindings_];
    n      = NavBinding{};
    n.kind = kind;
    n.btn  = &b;
    last_binding_ = static_cast<int8_t>(num_bindings_);
    num_bindings_++;
    return &n;
}

void Pager::AddNavPage(NavBinding& n, uint8_t page)
{
    if (page >= num_pages_ || n.num_pages >= kMaxPages) return;
    n.pages[n.num_pages++] = page;
}

Pager& Pager::Shift(IButton& b, uint8_t page)
{
    if (page >= num_pages_) { last_binding_ = -1; return *this; }
    if (NavBinding* n = AddBinding(NavKind::Shift, b))
    {
        n->pages[0]  = page;
        n->num_pages = 1u;
    }
    return *this;
}

Pager& Pager::Latch(IButton& b, uint8_t page_a, uint8_t page_b)
{
    if (page_a >= num_pages_ || page_b >= num_pages_ || page_a == page_b)
    {
        last_binding_ = -1;
        return *this;
    }
    if (NavBinding* n = AddBinding(NavKind::Latch, b))
    {
        n->pages[0]  = page_a;
        n->pages[1]  = page_b;
        n->num_pages = 2u;
        n->from_mask = static_cast<uint8_t>(PageBit(page_a) | PageBit(page_b));
        n->latch_sel = (page_ == page_b) ? 1u : 0u;
    }
    return *this;
}

/* ── Navigation queries / handshakes ───────────────────────────────────── */

void Pager::ConsumeButton() { consume_ = true; }

const IButton* Pager::PageButton() const
{
    for (uint8_t i = 0; i < num_bindings_; i++)
        if (bindings_[i].kind == NavKind::Cycle) return bindings_[i].btn;
    return nullptr;
}

const Pager::NavBinding* Pager::FindBinding(const IButton* b) const
{
    if (!b) return nullptr;
    for (uint8_t i = 0; i < num_bindings_; i++)
        if (bindings_[i].btn == b) return &bindings_[i];
    return nullptr;   /* buttons are unique per binding (AddBinding) */
}

bool Pager::ConsumesRelease(const IButton* b) const
{
    const NavBinding* n = FindBinding(b);
    return n && n->kind != NavKind::Shift;
}

bool Pager::HoldClaimed(const IButton* b) const
{
    const NavBinding* n = FindBinding(b);
    return n && n->kind == NavKind::Shift && n->active && n->used;
}

bool Pager::HoldUsed(const IButton& b) const
{
    const NavBinding* n = FindBinding(&b);
    return n && n->kind == NavKind::Shift && n->used;
}

LedPanel::Rgb Pager::IndicatorColor(const IButton* b) const
{
    const NavBinding* n = FindBinding(b);
    if (!n) return {0, 0, 0};
    switch (n->kind)
    {
        case NavKind::Cycle:
            return PageColor(page_);
        case NavKind::Shift:
            return n->active ? PageColor(n->pages[0]) : LedPanel::Rgb{0, 0, 0};
        case NavKind::Latch:
            return (page_ == n->pages[0] || page_ == n->pages[1])
                       ? PageColor(page_)
                       : LedPanel::Rgb{0, 0, 0};
    }
    return {0, 0, 0};
}

void Pager::RetainPage()  { retained_ov_ = ActiveOverlay(); }
void Pager::ReleasePage() { retained_ov_ = nullptr; }

/* ── Poll: edge detection + poison bookkeeping only ────────────────────── */

void Pager::PollButtons(uint32_t /*t_ms*/, bool gated)
{
    if (gated && !prev_gated_)
    {
        /* Gate rise: layers abort now, latched edges drop, in-flight
         * presses poison until their release. */
        AbortOverlays();
        for (uint8_t i = 0; i < num_bindings_; i++)
        {
            bindings_[i].press_pending   = false;
            bindings_[i].release_pending = false;
        }
    }

    for (uint8_t i = 0; i < num_bindings_; i++)
    {
        NavBinding& b = bindings_[i];
        const bool pressed = b.btn->Pressed();
        const bool rising  = pressed && !b.prev_pressed;
        const bool falling = !pressed && b.prev_pressed;
        b.prev_pressed = pressed;

        if (gated || prev_gated_)
        {
            /* Sync only: presses under (or spanning) the gate poison. */
            if (pressed) b.poisoned = true;
            if (falling)
            {
                b.poisoned        = false;
                b.press_pending   = false;
                b.release_pending = false;
            }
            continue;
        }

        if (rising)
        {
            bool other_down = false;
            for (uint8_t j = 0; j < num_bindings_; j++)
                if (j != i && bindings_[j].btn->Pressed()) other_down = true;

            if (other_down)
            {
                /* Two nav buttons down = reaching for a chord:
                 * navigation stands down. */
                AbortOverlays();
                for (uint8_t j = 0; j < num_bindings_; j++)
                    if (bindings_[j].btn->Pressed())
                        bindings_[j].poisoned = true;
                b.poisoned = true;
            }
            else
            {
                b.poisoned      = false;
                b.press_pending = true;
            }
        }
        if (falling) b.release_pending = true;
    }

    prev_gated_ = gated;
}

/* ── Frame: apply pending navigation, then catch ───────────────────────── */

void Pager::Update(const float* phys, uint32_t /*t_ms*/)
{
    /* Lazy re-arm after a Deserialize: we now have a phys[] to lock
     * against.  A held layer stays engaged and reads as clean again. */
    if (pending_rearm_)
    {
        pending_rearm_ = false;
        for (uint8_t pg = 0; pg < num_pages_; pg++) LockPage(pg, phys);
        if (NavBinding* ov = ActiveOverlay())
        {
            SnapshotOverlay();
            ov->used = false;
        }
    }

    /* Catch re-arm owed by a poll-context abort. */
    if (relock_pending_)
    {
        relock_pending_ = false;
        LockPage(page_, phys);
    }

    for (uint8_t i = 0; i < num_bindings_; i++)
    {
        NavBinding& b = bindings_[i];
        const bool press   = b.press_pending;
        const bool release = b.release_pending;
        if (!press && !release) continue;
        b.press_pending = b.release_pending = false;

        if (b.kind == NavKind::Shift)
        {
            if (!b.active)
            {
                /* Idle; press+release in one frame is a tap, no layer. */
                b.used = false;
                if (press && !release) TryEngage(b, phys);
            }
            else if (b.lingering)
            {
                /* Button starts UP, so press first: re-press resumes
                 * the hold, a same-frame release ends it again. */
                if (press) b.lingering = false;
                if (press && release)
                {
                    if (retained_ov_ == &b) b.lingering = true;
                    else                    ExitShift(b, phys);
                }
            }
            else
            {
                /* Held: release exits (defers under the view latch). */
                if (release)
                {
                    if (retained_ov_ == &b) b.lingering = true;
                    else                    ExitShift(b, phys);
                }
                if (press)
                {
                    if (b.active) b.lingering = false;
                    else          TryEngage(b, phys);
                }
            }
            if (release) b.poisoned = false;
            continue;
        }

        /* Cycle / Latch act on the release; the consume claim is
         * frame-wide. */
        if (!release) continue;
        if (b.poisoned)      { b.poisoned = false; continue; }
        if (consume_)        continue;
        if (ActiveOverlay()) continue;   /* navigation waits out a held layer */
        if (b.kind == NavKind::Cycle) DoCycle(b, phys);
        else                          DoLatch(b, phys);
    }

    /* Complete deferred exits once the view latch no longer covers them. */
    for (uint8_t i = 0; i < num_bindings_; i++)
    {
        NavBinding& b = bindings_[i];
        if (b.lingering && retained_ov_ != &b && !b.btn->Pressed())
            ExitShift(b, phys);
    }

    /* Pot-catch on the visible page — deliberately AFTER the releases:
     * the release frame never absorbs pot motion, so a final-millisecond
     * nudge cannot both edit a layer and pass for a clean tap. */
    PotState* row = &states_[page_][0];
    for (uint8_t p = 0; p < num_pots_; p++)
        UpdateCatch(row[p], phys[p]);

    /* Used-tracking: a pot that took hold counts; one caught at engage
     * counts once it moves. */
    NavBinding* ov = ActiveOverlay();
    if (ov && !ov->used && ov->pages[0] == page_)
    {
        for (uint8_t p = 0; p < num_pots_; p++)
        {
            if (overlay_caught_mask_ & (1u << p))
            {
                const float d = row[p].stored - overlay_engage_[p];
                if (d > kCatchTolerance || d < -kCatchTolerance)
                {
                    ov->used = true;
                    break;
                }
            }
            else if (row[p].caught)
            {
                ov->used = true;
                break;
            }
        }
    }

    /* Same-frame contract: an unmatched claim evaporates. */
    consume_ = false;
}

/* ── Navigation transitions ────────────────────────────────────────────── */

Pager::NavBinding* Pager::ActiveOverlay()
{
    for (uint8_t i = 0; i < num_bindings_; i++)
        if (bindings_[i].kind == NavKind::Shift && bindings_[i].active)
            return &bindings_[i];
    return nullptr;
}

const Pager::NavBinding* Pager::ActiveOverlay() const
{
    return const_cast<Pager*>(this)->ActiveOverlay();
}

void Pager::TryEngage(NavBinding& b, const float* phys)
{
    if (b.poisoned) return;
    if (ActiveOverlay()) { b.poisoned = true; return; } /* one layer at a time */
    if (!(b.from_mask & PageBit(page_))) return;        /* out of scope: inert */
    if (b.pages[0] == page_) return;                    /* already showing     */
    EngageShift(b, phys);
}

void Pager::EngageShift(NavBinding& b, const float* phys)
{
    b.return_page = page_;
    b.active      = true;
    b.used        = false;
    b.lingering   = false;
    page_ = b.pages[0];
    LockPage(page_, phys);
    SyncLatchSel(page_);
    SnapshotOverlay();
}

void Pager::ExitShift(NavBinding& b, const float* phys)
{
    b.active    = false;
    b.lingering = false;
    if (retained_ov_ == &b) retained_ov_ = nullptr;
    page_ = b.return_page;
    LockPage(page_, phys);
    /* b.used stands until the next press — HoldUsed() reads it across
     * the release frame. */
}

void Pager::SnapshotOverlay()
{
    overlay_caught_mask_ = 0u;
    for (uint8_t p = 0; p < num_pots_; p++)
    {
        const PotState& s = states_[page_][p];
        if (s.caught) overlay_caught_mask_ |= static_cast<uint8_t>(1u << p);
        overlay_engage_[p] = s.stored;
    }
}

/* Poll-context cancel (no phys[]): the catch re-arm is owed to the next
 * Update via relock_pending_ — safe, catch only runs in Update. */
void Pager::AbortOverlays()
{
    retained_ov_ = nullptr;
    for (uint8_t i = 0; i < num_bindings_; i++)
    {
        NavBinding& b = bindings_[i];
        if (b.kind != NavKind::Shift || !b.active) continue;
        b.active    = false;
        b.lingering = false;
        page_       = b.return_page;
        relock_pending_ = true;
    }
}

void Pager::DoCycle(NavBinding& b, const float* phys)
{
    /* An empty list is the classic gesture: every page, in order. */
    const uint8_t n = b.num_pages ? b.num_pages : num_pages_;
    const auto entry = [&](uint8_t i) -> uint8_t
    { return b.num_pages ? b.pages[i] : i; };

    int pos = -1;   /* current page's slot; a latch pair is one slot */
    for (uint8_t i = 0; i < n && pos < 0; i++)
        if (SameSlot(entry(i), page_)) pos = static_cast<int>(i);

    /* Advance to the first entry that LEAVES the current slot — in an
     * implicit list a latch pair appears as two entries, and the sibling
     * resolves straight back to the remembered member. */
    uint8_t next = entry(static_cast<uint8_t>((pos + 1) % n));
    for (uint8_t step = 1; step <= n; step++)
    {
        const uint8_t cand = entry(static_cast<uint8_t>((pos + step) % n));
        if (!SameSlot(cand, page_))
        {
            next = cand;
            break;
        }
    }
    Go(ResolveLatch(next), phys);
}

void Pager::DoLatch(NavBinding& b, const float* phys)
{
    if (!(b.from_mask & PageBit(page_))) return;
    uint8_t member;
    if      (page_ == b.pages[0]) member = 1u;
    else if (page_ == b.pages[1]) member = 0u;
    else return;                             /* outside the pair: inert */
    Go(b.pages[member], phys);   /* SyncLatchSel records the flip */
}

void Pager::Go(uint8_t page, const float* phys)
{
    page_ = page;
    /* Lock the new page so the user must catch each pot. */
    LockPage(page_, phys);
    SyncLatchSel(page_);
}

bool Pager::SameSlot(uint8_t entry, uint8_t cur) const
{
    if (entry == cur) return true;
    for (uint8_t i = 0; i < num_bindings_; i++)
    {
        const NavBinding& L = bindings_[i];
        if (L.kind != NavKind::Latch) continue;
        const bool has_e = (L.pages[0] == entry || L.pages[1] == entry);
        const bool has_c = (L.pages[0] == cur   || L.pages[1] == cur);
        if (has_e && has_c) return true;
    }
    return false;
}

uint8_t Pager::ResolveLatch(uint8_t page) const
{
    for (uint8_t i = 0; i < num_bindings_; i++)
    {
        const NavBinding& L = bindings_[i];
        if (L.kind != NavKind::Latch) continue;
        if (L.pages[0] == page || L.pages[1] == page)
            return L.pages[L.latch_sel];
    }
    return page;
}

void Pager::SyncLatchSel(uint8_t page)
{
    for (uint8_t i = 0; i < num_bindings_; i++)
    {
        NavBinding& L = bindings_[i];
        if (L.kind != NavKind::Latch) continue;
        if      (L.pages[0] == page) L.latch_sel = 0u;
        else if (L.pages[1] == page) L.latch_sel = 1u;
    }
}

/* ── Storage reads / writes (unchanged) ────────────────────────────────── */

float Pager::Value(uint8_t pot) const
{
    if (pot >= num_pots_) return 0.0f;
    return states_[page_][pot].stored;
}

float Pager::Stored(uint8_t page, uint8_t pot) const
{
    if (page >= num_pages_ || pot >= num_pots_) return 0.0f;
    return states_[page][pot].stored;
}

bool Pager::Caught(uint8_t pot) const
{
    if (pot >= num_pots_) return true;
    return states_[page_][pot].caught;
}

PotState Pager::State(uint8_t page, uint8_t pot) const
{
    if (page >= num_pages_ || pot >= num_pots_) return PotState{};
    return states_[page][pot];
}

void Pager::SetStored(uint8_t page, uint8_t pot, float value, const float* phys)
{
    if (page >= num_pages_ || pot >= num_pots_) return;
    PotState& s = states_[page][pot];
    s.stored = value;
    InitCatch(s, phys[pot]);
}

void Pager::LockPage(uint8_t page, const float* phys)
{
    if (page >= num_pages_) return;
    for (uint8_t p = 0; p < num_pots_; p++)
        InitCatch(states_[page][p], phys[p]);
}

void Pager::GoToPage(uint8_t page, const float* phys)
{
    if (page >= num_pages_) return;
    /* An explicit jump cancels any layer outright — no return leg. */
    retained_ov_ = nullptr;
    for (uint8_t i = 0; i < num_bindings_; i++)
    {
        NavBinding& b = bindings_[i];
        if (b.kind == NavKind::Shift)
        {
            b.active    = false;
            b.lingering = false;
        }
    }
    Go(page, phys);
}

void Pager::SetPageColor(uint8_t page, LedPanel::Rgb c)
{
    if (page >= kMaxPages) return;
    page_colors_[page] = c;
}

LedPanel::Rgb Pager::PageColor(uint8_t page) const
{
    if (page >= kMaxPages) return {0, 0, 0};
    return page_colors_[page];
}

/* ── Serializable ──────────────────────────────────────────────────────── */

size_t Pager::SerializedSize() const
{
    /* num_pages × num_pots floats (stored values only). */
    return static_cast<size_t>(num_pages_) * num_pots_ * sizeof(float);
}

void Pager::Serialize(uint8_t* out) const
{
    for (uint8_t pg = 0; pg < num_pages_; pg++)
    {
        for (uint8_t p = 0; p < num_pots_; p++)
        {
            const float v = states_[pg][p].stored;
            std::memcpy(out, &v, sizeof(float));
            out += sizeof(float);
        }
    }
}

bool Pager::Deserialize(const uint8_t* in)
{
    for (uint8_t pg = 0; pg < num_pages_; pg++)
    {
        for (uint8_t p = 0; p < num_pots_; p++)
        {
            float v;
            std::memcpy(&v, in, sizeof(float));
            in += sizeof(float);
            states_[pg][p].stored = v;
            /* Catch is locked here; pending_rearm_ will re-init on next Update. */
            states_[pg][p].caught       = false;
            states_[pg][p].sign_at_lock = 0;
        }
    }
    pending_rearm_ = true;
    return true;
}

uint32_t Pager::SchemaHash() const
{
    return kPagerSchemaTag
         ^ (static_cast<uint32_t>(num_pages_) << 8)
         ^ (static_cast<uint32_t>(num_pots_)  << 16)
         ^ static_cast<uint32_t>(sizeof(float));
}

} // namespace alchemy
