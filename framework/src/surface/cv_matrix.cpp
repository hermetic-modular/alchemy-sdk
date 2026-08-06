/**
 * @file surface/cv_matrix.cpp
 * @brief alchemy::CvMatrix implementation.
 *
 */

#include "alchemy/surface/cv_matrix.h"

#include "alchemy/surface/virtual_knob.h"

namespace alchemy {

/* ── JackBuilder ───────────────────────────────────────────────────────── */

CvMatrix::JackBuilder& CvMatrix::JackBuilder::To(VirtualKnob& knob)
{
    if (m_ && jack_ < m_->num_jacks_)
    {
        JackState& j = m_->jacks_[jack_];
        j.kind = Kind::Knob;
        j.knob = &knob;
        j.fn   = nullptr;
        j.ctx  = nullptr;
    }
    return *this;
}

CvMatrix::JackBuilder& CvMatrix::JackBuilder::Off()
{
    if (m_ && jack_ < m_->num_jacks_)
    {
        JackState& j = m_->jacks_[jack_];
        j.kind = Kind::Off;
        j.knob = nullptr;
        j.fn   = nullptr;
        j.ctx  = nullptr;
    }
    return *this;
}

CvMatrix::JackBuilder& CvMatrix::JackBuilder::Custom(CustomFn fn, void* ctx)
{
    if (m_ && jack_ < m_->num_jacks_)
    {
        JackState& j = m_->jacks_[jack_];
        j.kind = Kind::Custom;
        j.knob = nullptr;
        j.fn   = fn;
        j.ctx  = ctx;
    }
    return *this;
}

CvMatrix::JackBuilder& CvMatrix::JackBuilder::Atten(float v)
{
    if (m_ && jack_ < m_->num_jacks_) m_->jacks_[jack_].atten = v;
    return *this;
}

CvMatrix::JackBuilder& CvMatrix::JackBuilder::Bipolar(bool b)
{
    if (m_ && jack_ < m_->num_jacks_) m_->jacks_[jack_].bipolar = b;
    return *this;
}

/* ── CvMatrix ──────────────────────────────────────────────────────────── */

CvMatrix::CvMatrix(uint8_t num_jacks)
    : num_jacks_(num_jacks < kMaxJacks ? num_jacks : kMaxJacks)
{
}

CvMatrix::JackBuilder CvMatrix::Jack(uint8_t jack)
{
    return JackBuilder(this, jack);
}

float CvMatrix::DeltaAtPage(uint8_t page, uint8_t pot) const
{
    if (page >= kMaxPages || pot >= kMaxPots) return 0.0f;
    return delta_cache_[page][pot];
}

void CvMatrix::PollEdges(const float* cv, uint32_t t_us)
{
    if (!cv) return;
    for (uint8_t i = 0; i < num_jacks_; i++)
    {
        const JackState& j = jacks_[i];
        if (j.kind == Kind::Custom && j.fn)
            j.fn(cv[i], t_us, j.ctx);
    }
}

void CvMatrix::Update(const float* cv, uint32_t /*t_ms*/)
{
    /* Zero the cache.  All-bits-zero is IEEE 754 0.0f, so a tight loop
     * is fine; staying off <cstring> keeps the dependency surface small. */
    for (uint8_t p = 0; p < kMaxPages; p++)
        for (uint8_t q = 0; q < kMaxPots; q++)
            delta_cache_[p][q] = 0.0f;

    if (!cv) return;

    for (uint8_t i = 0; i < num_jacks_; i++)
    {
        const JackState& j = jacks_[i];
        if (j.kind != Kind::Knob || !j.knob) continue;

        const uint8_t page = j.knob->Page();
        const uint8_t pot  = j.knob->Pot();
        if (page >= kMaxPages || pot >= kMaxPots) continue;

        const float raw = cv[i];
        const float v   = j.bipolar ? (raw - 0.5f) : raw;
        delta_cache_[page][pot] += v * j.atten;
    }
}

} // namespace alchemy
