/**
 * @file alchemy/host_link/host.h
 * @brief hostlink::Host — batteries-included HostLink integration.
 *
 * One declaration and one Use() make a module manageable from the web
 * editor:
 *
 *   static Presets        presets(hw.seed.qspi);
 *   static hostlink::Host host(presets, "wavefolder", "Wavefolder",
 *                              "1.2.0", GIT_HASH);
 *
 *   int main()
 *   {
 *       ...
 *       presets.Manage(pager);
 *       presets.Manage(settings);
 *       presets.UseNames();
 *
 *       loop.Use(pager).Use(settings).Use(page_a).Use(page_b).Use(host);
 *
 *       presets.Init();
 *       presets.BootLoad();      // host starts here: descriptor + USB up
 *       ...
 *   }
 *
 * Everything is defaulted and overridable:
 *   - transport: USB CDC on the panel USB-C; product string = module
 *     display name (.Product() / .Transport() to override)
 *   - buffers: SDK-owned, in SDRAM (.Buffers()/.DescriptorBuffer())
 *   - descriptor: derived from the managed components + declared pages
 *     (see describe.h); override per component with OnDescribe(), or
 *     entirely with Descriptor()
 *   - uid / reboot: MCU unique id, bootloader/System reset (.Uid(),
 *     .OnReboot())
 *
 * Start timing: the host arms itself via the Presets pre-BootLoad hook,
 * so the descriptor's captured defaults are factory defaults regardless
 * of call order — attach pages (loop.Use) before presets.BootLoad() and
 * there is nothing else to sequence.  Without a ControlLoop, call
 * Poll(t_ms) from your own loop (same thread as preset mutations) —
 * the first Poll starts the link if BootLoad never did.
 *
 * One Host per system (the CDC receive callback is a hardware
 * singleton).  Target-only; host builds exercise the descriptor layer
 * via describe.h directly.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "alchemy/host_link/describe.h"
#include "alchemy/host_link/host_link.h"
#include "alchemy/surface/host_service.h"

namespace alchemy {

class ControlLoop;
class Page;
class Presets;
class VirtualButton;

namespace hostlink {

class Host : public HostService
{
  public:
    /**
     * @param id    Stable machine id (never rename; hosts key on it).
     * @param name  Display name (also the default USB product string).
     * @param fw_version / @p git_hash  Stamped into HELLO + descriptor.
     * All strings must be literals (kept by reference).
     */
    Host(Presets& presets, const char* id, const char* name,
         const char* fw_version, const char* git_hash);

    /* ── Optional configuration (call before the link starts) ───────── */

    /** USB product string (default: the module display name). */
    Host& Product(const char* usb_product);

    /** Slot auto-loaded at power-on, reported in HELLO (default 0). */
    Host& BootSlot(uint8_t slot);

    /** Custom staging/snapshot placement (default: SDK-owned SDRAM).
     *  @p cap must be ≥ the module's live serialized size. */
    Host& Buffers(uint8_t* staging, uint8_t* snapshot, size_t cap);

    /** Custom descriptor buffer (default: SDK-owned 8 KiB in SDRAM). */
    Host& DescriptorBuffer(char* buf, size_t cap);

    /** Custom byte transport (default: CDC on the panel USB-C). */
    Host& Transport(IHostTransport& transport);

    /** 96-bit unique id override (default: the MCU UID registers). */
    Host& Uid(const uint8_t uid[12]);

    /** Reboot handler override (default: DAISY bootloader / NVIC reset). */
    Host& OnReboot(void (*fn)(uint8_t mode, void* ctx), void* ctx);

    /**
     * Override the auto-description of one managed component (standard
     * surface or not) — @p fn writes it via the ComponentWriter and
     * returns true.  See describe.h.
     */
    Host& OnDescribe(const Serializable& component,
                     bool (*fn)(ComponentWriter&, void*),
                     void* ctx = nullptr);

    /** Hand-rolled descriptor (skips auto-derivation entirely).  The
     *  buffer must stay valid for the lifetime of the link. */
    Host& Descriptor(const void* json, uint32_t len);

    /** Explicit page metadata source when no ControlLoop is attached. */
    template <typename... Rest>
    Host& Pages(const Page& first, const Rest&... rest)
    {
        AddPage(first);
        (AddPage(rest), ...);
        return *this;
    }

    /**
     * Declare the module's physical push buttons — identity, role,
     * gestures, and (for State-role buttons) the preset fields they
     * mutate.  Emitted as the descriptor's top-level "buttons" array
     * (protocol §5.5).  @p buttons must outlive the link (a static
     * or constexpr file-scope table is the intended pattern).  Pass
     * count == 0 to omit the key entirely.
     */
    Host& Buttons(const VirtualButton* buttons, uint8_t count);

    /* ── Lifecycle ───────────────────────────────────────────────────── */

    /** Bring the link up (idempotent).  Called automatically from the
     *  Presets pre-BootLoad hook and from the first Poll(). */
    void Start();

    /** HostService: pump + execute (ControlLoop calls this at 1 ms). */
    void Poll(uint32_t t_ms) override;

    void OnAttach(ControlLoop& loop) override;

    /** The protocol engine, once started (escape hatch; else nullptr). */
    HostLink* Engine() { return link_; }

  private:
    static constexpr uint8_t kMaxOverrides = 8u;
    static constexpr uint8_t kMaxPageRefs  = 8u;

    static void PreBootTrampoline(void* self);
    void AddPage(const Page& p);

    Presets&    presets_;
    const char* id_;
    const char* name_;
    const char* fw_;
    const char* git_;

    const char* product_   = nullptr;
    uint8_t     boot_slot_ = 0u;

    uint8_t* staging_  = nullptr;
    uint8_t* snapshot_ = nullptr;
    size_t   buf_cap_  = 0u;
    char*    desc_buf_ = nullptr;
    size_t   desc_cap_ = 0u;

    IHostTransport* transport_ = nullptr;
    const uint8_t*  uid_       = nullptr;

    void (*reboot_fn_)(uint8_t, void*) = nullptr;
    void*  reboot_ctx_                 = nullptr;

    const void* manual_desc_     = nullptr;
    uint32_t    manual_desc_len_ = 0u;

    DescribeOverride overrides_[kMaxOverrides] = {};
    uint8_t          num_overrides_            = 0u;

    ControlLoop* loop_                  = nullptr;
    const Page*  pages_[kMaxPageRefs]   = {};
    uint8_t      num_pages_             = 0u;

    const VirtualButton* buttons_       = nullptr;
    uint8_t              num_buttons_   = 0u;

    HostLink* link_    = nullptr;
    bool      started_ = false;
    alignas(HostLink) uint8_t link_storage_[sizeof(HostLink)];
};

} // namespace hostlink
} // namespace alchemy
