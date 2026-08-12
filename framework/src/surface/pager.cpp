/**
 * @file surface/pager.cpp
 * @brief alchemy::Pager implementation.
 */

#include "alchemy/surface/pager.h"
#include <cstring>

namespace alchemy {

/* Schema tag — bump intentionally if the on-flash layout for Pager changes. */
static constexpr uint32_t kPagerSchemaTag = 0x50414730u; /* 'PAG0' */


Pager::Pager(IButton& b1, uint8_t num_pages, uint8_t num_pots)
    : b1_(&b1),
      num_pages_((num_pages == 0u) ? 1u
                 : (num_pages > kMaxPages ? kMaxPages : num_pages)),
      num_pots_((num_pots == 0u) ? 1u
                : (num_pots > kMaxPots ? kMaxPots : num_pots))
{
    for (uint8_t pg = 0; pg < kMaxPages; pg++)
        for (uint8_t p = 0; p < kMaxPots; p++)
            states_[pg][p] = PotState{0.5f, true, 0};
}

void Pager::ConsumeButton() { consume_ = true; }

void Pager::PollButtons(uint32_t /*t_ms*/, bool gated)
{
    const bool pressed = b1_->Pressed();
    const bool falling = !pressed && prev_pressed_;
    prev_pressed_ = pressed;
    /* When gated (e.g. Settings active), keep prev synced but don't react. */
    if (gated) return;
    if (falling) advance_pending_ = true;
}

void Pager::Update(const float* phys, uint32_t /*t_ms*/)
{
    /* Lazy re-arm after a Deserialize: we now have a phys[] to lock against. */
    if (pending_rearm_)
    {
        pending_rearm_ = false;
        for (uint8_t pg = 0; pg < num_pages_; pg++) LockPage(pg, phys);
    }

    if (advance_pending_)
    {
        advance_pending_ = false;
        if (consume_)
        {
            consume_ = false;
        }
        else
        {
            page_ = static_cast<uint8_t>((page_ + 1u) % num_pages_);
            /* Lock the new page so the user must catch each pot. */
            LockPage(page_, phys);
        }
    }

    /* Run pot-catch on the currently visible page. */
    PotState* row = &states_[page_][0];
    for (uint8_t p = 0; p < num_pots_; p++)
        UpdateCatch(row[p], phys[p]);
}

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
    page_ = page;
    /* Same catch contract as the advance branch in Update(). */
    LockPage(page_, phys);
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
