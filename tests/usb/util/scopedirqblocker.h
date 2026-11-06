#pragma once
#include <cstdint>
inline uint32_t test_primask = 0;
namespace daisy {
class ScopedIrqBlocker {
  public:
    ScopedIrqBlocker() : previous_(test_primask) { test_primask = 1; }
    ~ScopedIrqBlocker() { test_primask = previous_; }
  private:
    uint32_t previous_;
};
}
