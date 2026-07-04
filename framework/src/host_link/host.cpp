/**
 * @file host_link/host.cpp
 * @brief hostlink::Host implementation (target-only: CDC + HAL UID).
 */

#include "alchemy/host_link/host.h"

#include <cstring>
#include <new>

#include "daisy_seed.h"
#include "stm32h7xx_hal.h"   /* HAL_GetUIDw*, HAL_NVIC_SystemReset */

#include "alchemy/host_link/cdc_transport.h"
#include "alchemy/surface/control_loop.h"
#include "alchemy/surface/page.h"
#include "alchemy/surface/presets.h"
#include "alchemy/version.h"

namespace alchemy {
namespace hostlink {

/* One Host per system (the CDC callback is a hardware singleton), so
 * SDK-owned default buffers are safe to share.  SDRAM: big, cold, only
 * touched during host transactions — and not zeroed by startup, hence
 * the memsets in Start(). */
static char    DSY_SDRAM_BSS s_descriptor[8u * 1024u];
static uint8_t DSY_SDRAM_BSS s_staging [kPresetBlobCapacity];
static uint8_t DSY_SDRAM_BSS s_snapshot[kPresetBlobCapacity];

static CdcUsbTransport  s_cdc;
static daisy::UsbHandle s_usb;

static void DefaultReboot(uint8_t mode, void*)
{
    if (mode == kRebootBootloader)
        daisy::System::ResetToBootloader(daisy::System::BootloaderMode::DAISY);
    else
        HAL_NVIC_SystemReset();
    for (;;) {}   /* unreachable; both paths reset */
}

Host::Host(Presets& presets, const char* id, const char* name,
           const char* fw_version, const char* git_hash)
    : presets_(presets), id_(id), name_(name), fw_(fw_version), git_(git_hash)
{
    /* Arm the pre-BootLoad hook: the descriptor renders while components
     * still hold factory defaults, whatever the app's call order. */
    presets_.SetPreBootHook(&PreBootTrampoline, this);
}

void Host::PreBootTrampoline(void* self)
{
    static_cast<Host*>(self)->Start();
}

Host& Host::Product(const char* usb_product) { product_ = usb_product; return *this; }
Host& Host::BootSlot(uint8_t slot)           { boot_slot_ = slot; return *this; }

Host& Host::Buffers(uint8_t* staging, uint8_t* snapshot, size_t cap)
{
    staging_  = staging;
    snapshot_ = snapshot;
    buf_cap_  = cap;
    return *this;
}

Host& Host::DescriptorBuffer(char* buf, size_t cap)
{
    desc_buf_ = buf;
    desc_cap_ = cap;
    return *this;
}

Host& Host::Transport(IHostTransport& transport) { transport_ = &transport; return *this; }
Host& Host::Uid(const uint8_t uid[12])           { uid_ = uid; return *this; }

Host& Host::OnReboot(void (*fn)(uint8_t, void*), void* ctx)
{
    reboot_fn_  = fn;
    reboot_ctx_ = ctx;
    return *this;
}

Host& Host::OnDescribe(const Serializable& component,
                       bool (*fn)(ComponentWriter&, void*), void* ctx)
{
    if (num_overrides_ < kMaxOverrides)
        overrides_[num_overrides_++] = DescribeOverride{&component, fn, ctx};
    return *this;
}

Host& Host::Descriptor(const void* json, uint32_t len)
{
    manual_desc_     = json;
    manual_desc_len_ = len;
    return *this;
}

void Host::AddPage(const Page& p)
{
    if (num_pages_ < kMaxPageRefs) pages_[num_pages_++] = &p;
}

void Host::OnAttach(ControlLoop& loop) { loop_ = &loop; }

void Host::Start()
{
    if (started_) return;
    started_ = true;

    if (!staging_)
    {
        std::memset(s_staging, 0, sizeof s_staging);
        std::memset(s_snapshot, 0, sizeof s_snapshot);
        staging_  = s_staging;
        snapshot_ = s_snapshot;
        buf_cap_  = sizeof s_staging;
    }
    if (!desc_buf_ && !manual_desc_)
    {
        std::memset(s_descriptor, 0, sizeof s_descriptor);
        desc_buf_ = s_descriptor;
        desc_cap_ = sizeof s_descriptor;
    }
    if (!transport_)
    {
        /* Alchemy Lab routes the panel USB-C to the Seed2 DFM's EXTERNAL
         * USB pins on V2 (the port the bootloader serves DFU on); V1
         * exposes the Seed's onboard port.  The product string is what
         * the OS and the browser's port picker display. */
#if defined(ALCHEMY_BOARD_V2)
        s_cdc.Init(s_usb, daisy::UsbHandle::FS_EXTERNAL,
                   product_ ? product_ : name_);
#else
        s_cdc.Init(s_usb, daisy::UsbHandle::FS_INTERNAL,
                   product_ ? product_ : name_);
#endif
        transport_ = &s_cdc;
    }

#if defined(ALCHEMY_BOARD_V2)
    constexpr uint8_t kBoardNum   = 2u;
    const char*       board_name  = "v2";
#else
    constexpr uint8_t kBoardNum   = 1u;
    const char*       board_name  = "v1";
#endif

    link_ = new (link_storage_) HostLink(
        *transport_, presets_,
        HostLink::Info{id_, name_, fw_, git_, ALCHEMY_SDK_VERSION,
                       kBoardNum, boot_slot_},
        staging_, snapshot_, buf_cap_);

    if (manual_desc_)
    {
        link_->SetDescriptor(manual_desc_, manual_desc_len_);
    }
    else
    {
        /* Page metadata: explicit .Pages() wins; else whatever the
         * attached ControlLoop has by now. */
        if (num_pages_ == 0u && loop_)
        {
            uint8_t n = loop_->NumAttachedPages();
            if (n > kMaxPageRefs) n = kMaxPageRefs;
            for (uint8_t i = 0; i < n; i++)
                pages_[num_pages_++] = loop_->AttachedPage(i);
        }
        const PageSet set{pages_, num_pages_};
        const uint32_t len = RenderDescriptor(
            desc_buf_, desc_cap_,
            DescriptorBuilder::ModuleInfo{id_, name_, fw_, git_,
                                          ALCHEMY_SDK_VERSION, board_name},
            presets_, num_pages_ ? &set : nullptr,
            overrides_, num_overrides_);
        link_->SetDescriptor(desc_buf_, len);
    }

    if (uid_)
    {
        link_->SetUid(uid_);
    }
    else
    {
        uint8_t uid[12];
        const uint32_t w0 = HAL_GetUIDw0();
        const uint32_t w1 = HAL_GetUIDw1();
        const uint32_t w2 = HAL_GetUIDw2();
        std::memcpy(uid + 0, &w0, 4);
        std::memcpy(uid + 4, &w1, 4);
        std::memcpy(uid + 8, &w2, 4);
        link_->SetUid(uid);
    }

    link_->SetRebootHandler(reboot_fn_ ? reboot_fn_ : &DefaultReboot,
                            reboot_fn_ ? reboot_ctx_ : nullptr);
}

void Host::Poll(uint32_t t_ms)
{
    if (!started_) Start();
    link_->Poll(t_ms);
}

} // namespace hostlink
} // namespace alchemy
