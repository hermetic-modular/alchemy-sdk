/**
 * @file alchemy/host_link/json_writer.h
 * @brief Minimal allocation-free JSON emitter for descriptor rendering.
 *
 * No printf float formatting (newlib-nano's %f is not linked by default):
 * Float() renders via integer math at 4 decimal places, trailing zeros
 * trimmed — ample for pot-normalized values and ranges.
 *
 * Comma placement is automatic; on overflow the writer latches a failure
 * flag and Ok() returns false (output must then be discarded).
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace alchemy {
namespace hostlink {

class JsonWriter
{
  public:
    JsonWriter(char* buf, size_t cap) : buf_(buf), cap_(cap) {}

    /* ── Structure ──────────────────────────────────────────────── */

    void BeginObj() { Sep(); Put('{'); Push(); }
    void EndObj()   { Put('}'); Pop(); }
    void BeginArr() { Sep(); Put('['); Push(); }
    void EndArr()   { Put(']'); Pop(); }

    void Key(const char* k)
    {
        Sep();
        Quoted(k);
        Put(':');
        pending_value_ = true;
    }

    /* ── Values ─────────────────────────────────────────────────── */

    void Str(const char* s)       { Sep(); Quoted(s ? s : ""); }
    void Bool(bool v)             { Sep(); Puts(v ? "true" : "false"); }
    void UInt(uint32_t v)         { Sep(); PutUInt(v); }
    void Int(int32_t v)
    {
        Sep();
        if (v < 0) { Put('-'); PutUInt(static_cast<uint32_t>(-(int64_t)v)); }
        else       { PutUInt(static_cast<uint32_t>(v)); }
    }

    /** 4-decimal fixed rendering, trailing zeros trimmed ("0.61", "16"). */
    void Float(float v)
    {
        Sep();
        if (v < 0.0f) { Put('-'); v = -v; }
        if (v > 400000.0f) v = 400000.0f;      /* keep integer math safe */
        const uint32_t scaled = static_cast<uint32_t>(v * 10000.0f + 0.5f);
        PutUInt(scaled / 10000u);
        uint32_t frac = scaled % 10000u;
        if (frac != 0u)
        {
            Put('.');
            char d[4] = { char('0' + frac / 1000u),
                          char('0' + (frac / 100u) % 10u),
                          char('0' + (frac / 10u) % 10u),
                          char('0' + frac % 10u) };
            int last = 3;
            while (last >= 0 && d[last] == '0') last--;
            for (int i = 0; i <= last; i++) Put(d[i]);
        }
    }

    /** Splice a prebuilt JSON value verbatim (caller guarantees validity). */
    void RawValue(const char* json) { Sep(); Puts(json); }

    /* ── State ──────────────────────────────────────────────────── */

    bool   Ok()     const { return !overflow_ && depth_ == 0; }
    size_t Length() const { return len_; }
    const char* Data() const { return buf_; }
    /** NUL-terminate (not counted in Length()); false on overflow. */
    bool Terminate()
    {
        if (len_ >= cap_) { overflow_ = true; return false; }
        buf_[len_] = '\0';
        return !overflow_;
    }

  private:
    static constexpr int kMaxDepth = 12;

    void Put(char c)
    {
        if (len_ < cap_) buf_[len_++] = c;
        else             overflow_ = true;
    }
    void Puts(const char* s) { while (*s) Put(*s++); }
    void PutUInt(uint32_t v)
    {
        char tmp[10];
        int  n = 0;
        do { tmp[n++] = char('0' + v % 10u); v /= 10u; } while (v);
        while (n) Put(tmp[--n]);
    }
    void Quoted(const char* s)
    {
        Put('"');
        for (; *s; s++)
        {
            const unsigned char c = static_cast<unsigned char>(*s);
            if (c == '"' || c == '\\') { Put('\\'); Put(char(c)); }
            else if (c < 0x20u)
            {
                Puts("\\u00");
                const char hex[] = "0123456789abcdef";
                Put(hex[(c >> 4) & 0xF]);
                Put(hex[c & 0xF]);
            }
            else Put(char(c));
        }
        Put('"');
    }

    void Sep()
    {
        if (pending_value_) { pending_value_ = false; return; }
        if (depth_ > 0)
        {
            if (has_elem_[depth_ - 1]) Put(',');
            has_elem_[depth_ - 1] = true;
        }
    }
    void Push()
    {
        if (depth_ < kMaxDepth) has_elem_[depth_] = false;
        depth_++;
        if (depth_ > kMaxDepth) overflow_ = true;
    }
    void Pop() { if (depth_ > 0) depth_--; }

    char*  buf_;
    size_t cap_;
    size_t len_           = 0u;
    int    depth_         = 0;
    bool   has_elem_[kMaxDepth] = {};
    bool   pending_value_ = false;
    bool   overflow_      = false;
};

} // namespace hostlink
} // namespace alchemy
