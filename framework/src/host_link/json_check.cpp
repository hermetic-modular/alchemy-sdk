#include "alchemy/host_link/json_check.h"

#include <cstring>

namespace alchemy {
namespace hostlink {

namespace {

constexpr int kMaxDepth = 16;

struct Cursor
{
    const char* p;
    const char* end;
};

void SkipWs(Cursor& c)
{
    while (c.p < c.end
           && (*c.p == ' ' || *c.p == '\t' || *c.p == '\n' || *c.p == '\r'))
        c.p++;
}

bool ParseValue(Cursor& c, int depth);

bool ParseHex4(Cursor& c)
{
    for (int i = 0; i < 4; i++)
    {
        if (c.p >= c.end) return false;
        const char ch = *c.p++;
        const bool hex = (ch >= '0' && ch <= '9')
                      || (ch >= 'a' && ch <= 'f')
                      || (ch >= 'A' && ch <= 'F');
        if (!hex) return false;
    }
    return true;
}

bool ParseString(Cursor& c)
{
    if (c.p >= c.end || *c.p != '"') return false;
    c.p++;
    while (c.p < c.end)
    {
        const unsigned char ch = static_cast<unsigned char>(*c.p++);
        if (ch == '"') return true;
        if (ch == '\\')
        {
            if (c.p >= c.end) return false;
            const char e = *c.p++;
            switch (e)
            {
                case '"': case '\\': case '/': case 'b': case 'f':
                case 'n': case 'r': case 't':
                    break;
                case 'u':
                    if (!ParseHex4(c)) return false;
                    break;
                default:
                    return false;
            }
        }
        else if (ch < 0x20u)
        {
            return false;
        }
    }
    return false;
}

bool ParseDigits(Cursor& c)
{
    if (c.p >= c.end || *c.p < '0' || *c.p > '9') return false;
    while (c.p < c.end && *c.p >= '0' && *c.p <= '9') c.p++;
    return true;
}

bool ParseNumber(Cursor& c)
{
    if (c.p < c.end && *c.p == '-') c.p++;
    if (c.p >= c.end) return false;
    if (*c.p == '0') c.p++;
    else if (!ParseDigits(c)) return false;
    if (c.p < c.end && *c.p == '.')
    {
        c.p++;
        if (!ParseDigits(c)) return false;
    }
    if (c.p < c.end && (*c.p == 'e' || *c.p == 'E'))
    {
        c.p++;
        if (c.p < c.end && (*c.p == '+' || *c.p == '-')) c.p++;
        if (!ParseDigits(c)) return false;
    }
    return true;
}

bool ParseLiteral(Cursor& c, const char* lit)
{
    const size_t n = std::strlen(lit);
    if (static_cast<size_t>(c.end - c.p) < n) return false;
    if (std::memcmp(c.p, lit, n) != 0) return false;
    c.p += n;
    return true;
}

bool ParseObject(Cursor& c, int depth)
{
    c.p++;
    SkipWs(c);
    if (c.p < c.end && *c.p == '}') { c.p++; return true; }
    for (;;)
    {
        SkipWs(c);
        if (!ParseString(c)) return false;
        SkipWs(c);
        if (c.p >= c.end || *c.p != ':') return false;
        c.p++;
        if (!ParseValue(c, depth)) return false;
        SkipWs(c);
        if (c.p >= c.end) return false;
        if (*c.p == ',') { c.p++; continue; }
        if (*c.p == '}') { c.p++; return true; }
        return false;
    }
}

bool ParseArray(Cursor& c, int depth)
{
    c.p++;
    SkipWs(c);
    if (c.p < c.end && *c.p == ']') { c.p++; return true; }
    for (;;)
    {
        if (!ParseValue(c, depth)) return false;
        SkipWs(c);
        if (c.p >= c.end) return false;
        if (*c.p == ',') { c.p++; continue; }
        if (*c.p == ']') { c.p++; return true; }
        return false;
    }
}

bool ParseValue(Cursor& c, int depth)
{
    if (depth >= kMaxDepth) return false;
    SkipWs(c);
    if (c.p >= c.end) return false;
    switch (*c.p)
    {
        case '{': return ParseObject(c, depth + 1);
        case '[': return ParseArray(c, depth + 1);
        case '"': return ParseString(c);
        case 't': return ParseLiteral(c, "true");
        case 'f': return ParseLiteral(c, "false");
        case 'n': return ParseLiteral(c, "null");
        default:  return ParseNumber(c);
    }
}

} // namespace

bool ValidJsonValue(const char* s, size_t len)
{
    if (!s) return false;
    Cursor c{s, s + len};
    if (!ParseValue(c, 0)) return false;
    SkipWs(c);
    return c.p == c.end;
}

bool ValidJsonValue(const char* s)
{
    return s != nullptr && ValidJsonValue(s, std::strlen(s));
}

} // namespace hostlink
} // namespace alchemy
