#include "alchemy/midi/pager_target.h"
#include "alchemy/midi/mono_options.h"
#include <cassert>
#include <cmath>

using namespace alchemy;
using namespace alchemy::midi;

static void ChannelSelection() {
    MonoOptions options;
    assert(options.Enabled() && options.Omni() && options.Channel() == 0);
    options.Disable(); assert(!options.Enabled() && options.Listening().channel_mask == 0);
    for (uint8_t ch = 1; ch <= 16; ++ch) {
        assert(options.SetChannel(ch) && options.Channel() == ch && !options.Omni());
        assert(options.Listening().channel_mask == Listen::Channel(ch).channel_mask);
    }
    assert(!options.SetChannel(0) && !options.SetChannel(17) && options.Channel() == 16);
    options.SetOmni(); assert(options.Omni() && options.Listening().channel_mask == 0xffff);
}

int main() {
    ChannelSelection();
    Pager pager(2, 2);
    float physical[2]{0.25f, 0.75f};
    pager.Update(physical, 0);
    PagerTargets<2> targets;
    assert(targets.Bind(1, pager, 0, 0));
    assert(targets.Bind(2, pager, 1, 1));
    assert(!targets.Bind(3, pager, 0, 0));
    assert(!targets.Bind(1, pager, 1, 0));
    ParameterCommand command;
    command.target = 1; command.value = 0.8f;
    assert(targets.Apply(command, physical));
    float value = 0;
    assert(targets.Read(1, value) && value == 0.8f);
    assert(!pager.Caught(0));
    pager.Update(physical, 10);
    assert(targets.Read(1, value) && value == 0.8f);
    physical[0] = 0.8f;
    pager.Update(physical, 20);
    assert(pager.Caught(0));
    command.operation = RouteOp::RelativeBase; command.value = 0.7f;
    assert(targets.Apply(command, physical));
    assert(targets.Read(1, value) && value == 1.f && !pager.Caught(0));
    command.operation = RouteOp::AddOffset;
    assert(!targets.Apply(command, physical));
    command.operation = RouteOp::EditBase; command.target = 2; command.value = 0.3f;
    assert(targets.Apply(command, physical));
    assert(targets.Read(2, value) && value == 0.3f);
    command.value = NAN;
    assert(!targets.Apply(command, physical));
}
