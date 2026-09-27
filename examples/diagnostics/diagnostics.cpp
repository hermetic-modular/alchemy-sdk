/** USB diagnostics without a preset store. Open hostlink-cli logs --follow
 * or the programmer's Device console. No Daisy logger setup is needed. */
#include <cmath>
#include "alchemy/hw/alchemy_lab.h"
#include "alchemy/host_link/host.h"
#include "alchemy/host_link/diagnostics.h"
#include "alchemy/surface/control_loop.h"

using namespace alchemy;
static AlchemyLab hw;
static ControlLoop loop(hw);
static hostlink::Host host("diagnostics_demo", "Diagnostics Demo", "1.0.0", "example");
static hostlink::Diagnostics debug;
static hostlink::Gauge<float> peak("audio.peak", "Output peak");
static hostlink::Gauge<uint32_t> uptime("system.uptime", "Uptime");

static void Audio(daisy::AudioHandle::InputBuffer in,
                  daisy::AudioHandle::OutputBuffer out, size_t size)
{
    float value = 0.f;
    for (size_t i = 0; i < size; ++i)
    {
        out[0][i] = in[0][i];
        out[1][i] = in[1][i];
        value = std::fmax(value, std::fmax(std::fabs(out[0][i]), std::fabs(out[1][i])));
    }
    peak.Set(value); // lock-free scalar; formatted logging stays in main()
}

int main()
{
    hw.Init();
    peak.Unit("FS");
    uptime.Unit("s");
    debug.Watch(peak);
    debug.Watch(uptime);
    host.Extend(debug);
    loop.Use(host);
    if (!host.ConfigurationOk() || !debug.ConfigurationOk())
        debug.Error("Diagnostics setup failed");
    debug.Info("Ready; stereo passthrough is running");
    hw.StartAudio(Audio);
    uint32_t last = 0;
    for (;;)
    {
        loop.Tick();
        const uint32_t now = daisy::System::GetNow();
        if (now - last >= 1000u)
        {
            last = now;
            uptime.Set(now / 1000u);
        }
    }
}
