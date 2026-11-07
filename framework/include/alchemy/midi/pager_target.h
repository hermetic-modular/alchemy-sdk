#pragma once

#include <array>
#include <cmath>
#include "alchemy/midi/routes.h"
#include "alchemy/surface/pager.h"

namespace alchemy::midi {

// Foreground bridge to stored, normalized panel values; CV and locks remain separate lanes.
template <size_t Capacity = 32>
class PagerTargets {
    struct Binding {
        ParamId id = 0;
        Pager* pager = nullptr;
        uint8_t page = 0, pot = 0;
    };
  public:
    bool Bind(ParamId id, Pager& pager, uint8_t page, uint8_t pot) {
        if (!id || page >= pager.NumPages() || pot >= pager.NumPots()) return false;
        for (const auto& binding : bindings_)
            if (binding.id == id || (binding.pager == &pager && binding.page == page && binding.pot == pot)) return false;
        for (auto& binding : bindings_) {
            if (binding.id) continue;
            binding = {id, &pager, page, pot};
            return true;
        }
        return false;
    }

    bool Read(ParamId id, float& value) const {
        for (const auto& binding : bindings_) {
            if (binding.id != id || !binding.pager) continue;
            value = binding.pager->Stored(binding.page, binding.pot);
            return std::isfinite(value);
        }
        return false;
    }

    bool Apply(const ParameterCommand& command, const float* physical_pots) {
        if (!std::isfinite(command.value) || !physical_pots) return false;
        for (const auto& binding : bindings_) {
            if (binding.id != command.target || !binding.pager) continue;
            if (!std::isfinite(physical_pots[binding.pot])) return false;
            float value = command.value;
            if (command.operation == RouteOp::RelativeBase) {
                const auto stored = binding.pager->Stored(binding.page, binding.pot);
                if (!std::isfinite(stored)) return false;
                value += stored;
            }
            else if (command.operation != RouteOp::EditBase) return false;
            value = value < 0.f ? 0.f : value > 1.f ? 1.f : value;
            binding.pager->SetStored(binding.page, binding.pot, value, physical_pots);
            return true;
        }
        return false;
    }

  private:
    std::array<Binding, Capacity> bindings_{};
};

} // namespace alchemy::midi
