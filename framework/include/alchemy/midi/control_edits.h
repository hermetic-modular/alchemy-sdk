#pragma once

#include <cmath>
#include "alchemy/midi/queue.h"
#include "alchemy/midi/routes.h"

namespace alchemy::midi {

struct ControlEdit {
    ParameterCommand command{};
    uint32_t revision = 0;
    uint32_t base_revision = 0;
};

// Panel/preset edits advance base_revision; MIDI receipts preserve it until the next publication.
template <size_t Capacity = 32>
class ControlEdits {
  public:
    bool PushBase(ParameterCommand command, float resolved_base, uint32_t revision, uint32_t base_revision) {
        if (!revision || !base_revision || !command.target || !command.route
            || command.source.port == kAnyPort || !command.source.session || command.channel > 15
            || !std::isfinite(resolved_base)
            || (command.operation != RouteOp::EditBase && command.operation != RouteOp::RelativeBase
                && command.operation != RouteOp::Select)) return false;
        if (command.operation != RouteOp::Select) command.operation = RouteOp::EditBase;
        command.value = resolved_base;
        return queue_.Push({command, revision, base_revision});
    }

    template <typename BaseRevision, typename Sink>
    size_t Drain(uint32_t revision, BaseRevision&& current_base_revision, Sink&& sink, size_t budget = Capacity) {
        size_t count = 0;
        ControlEdit edit;
        while (count < budget && queue_.Pop(edit)) {
            if (edit.revision == revision && edit.base_revision == current_base_revision(edit.command.target))
                sink(edit.command);
            ++count;
        }
        return count;
    }

    void Clear() { queue_.Clear(); }

  private:
    Queue<ControlEdit, Capacity> queue_;
};

} // namespace alchemy::midi
