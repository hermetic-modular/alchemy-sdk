#pragma once

#include <cstddef>

namespace alchemy {
namespace hostlink {

bool ValidJsonValue(const char* s, size_t len);
bool ValidJsonValue(const char* s);

} // namespace hostlink
} // namespace alchemy
