#pragma once

#include <cstddef>
#include <cstdint>
#include "alchemy/host_link/extension.h"
#include "alchemy/host_link/frame.h"
#include "alchemy/host_link/transport.h"
#include "alchemy/host_link/wire.h"
#include "alchemy/surface/presets.h"

namespace alchemy {
namespace hostlink {

class FastValues;

// Poll on the thread that owns preset mutations. The transport may buffer incoming bytes in an ISR.
// Publish a complete parameter snapshot to audio after applying a preset.
class HostLink
{
  public:
    struct Info
    {
        const char* module_id;    /* stable machine id, e.g. "echoa" */
        const char* module_name;  /* display name                    */
        const char* fw_version;   /* app semver                      */
        const char* fw_git;       /* short git hash                  */
        const char* sdk_version;  /* ALCHEMY_SDK_VERSION             */
        uint8_t     board;        /* 1 = v1, 2 = v2, 0 = unknown     */
        uint8_t     boot_slot;    /* slot auto-loaded at power-on    */
    };

    /**
     * @param staging / @p snapshot  Caller-owned buffers of @p buf_cap
     *   bytes each (host→device staged writes; device→host live
     *   snapshots).  Caller chooses placement — on RAM-tight targets put
     *   them in a slow/large region (e.g. SDRAM via DSY_SDRAM_BSS); they
     *   are only touched during host transactions.  @p buf_cap must be
     *   ≥ the module's live serialized size; writes are additionally
     *   capped at the slot capacity.
     */
    HostLink(IHostTransport& transport, Presets& presets, const Info& info,
             uint8_t* staging, uint8_t* snapshot, size_t buf_cap);

    /** Identity, reboot and extensions without a preset store. HELLO
     * reports zero slots/live size; preset operations are unsupported. */
    HostLink(IHostTransport& transport, const Info& info);

    /** Attach the pre-rendered descriptor JSON (see DescriptorBuilder).
     *  Pass len = 0 to advertise "no descriptor".  The buffer must stay
     *  valid and immutable until the next SetDescriptor(). Call only
     *  on the Poll() thread. Replacing the bytes invalidates an ongoing
     *  read: nonzero offsets return BadState until a new offset-zero
     *  request. Hosts restart with HELLO and verify its length/CRC. */
    void SetDescriptor(const void* json, uint32_t len);

    /** 96-bit MCU unique id, reported in HELLO. */
    void SetUid(const uint8_t uid[12]);

    /** Invoked ~100 ms after a REBOOT command is acknowledged.
     *  mode: kRebootApp | kRebootBootloader.  Must not return. */
    void SetRebootHandler(void (*fn)(uint8_t mode, void* ctx), void* ctx);

    /**
     * Register an optional command-block handler (protocol extension —
     * see extension.h).  Up to kMaxExtensions; a registration whose
     * range overlaps core commands or an earlier extension is ignored
     * (first claim wins).  Returns false when ignored.
     */
    bool Extend(IHostlinkExtension& ext);

    void SetFastValues(const FastValues& values) { fast_ = &values; }

    /** Pump the transport, then parse + execute pending requests.
     *  Call from the control loop (1 ms cadence recommended). */
    void Poll(uint32_t now_ms);

  public:
    static constexpr uint8_t kMaxExtensions = 4u;

  private:
    static constexpr uint8_t  kMaxFramesPerPoll = 8u;
    static constexpr uint32_t kStageTimeoutMs   = 5000u;
    static constexpr uint32_t kRebootDelayMs    = 100u;

    void Handle(const ParsedFrame& f, uint32_t now_ms);

    void OnHello        (const ParsedFrame& f);
    void OnGetDescriptor(const ParsedFrame& f);
    void OnListSlots    (const ParsedFrame& f);
    void OnReadSlot     (const ParsedFrame& f);
    void OnBlobBegin    (const ParsedFrame& f, uint32_t now_ms);
    void OnBlobData     (const ParsedFrame& f, uint32_t now_ms);
    void OnBlobCommit   (const ParsedFrame& f);
    void OnEraseSlot    (const ParsedFrame& f);
    void OnGetLive      (const ParsedFrame& f);
    void OnSaveToSlot   (const ParsedFrame& f);
    void OnLoadFromSlot (const ParsedFrame& f);
    void OnReboot       (const ParsedFrame& f, uint32_t now_ms);

    void RespondStatus(const ParsedFrame& f, Status s);
    void SendError(uint16_t seq, Status s);
    void Send(FrameWriter& w);
    void AbortStage() { stage_open_ = false; stage_recv_ = 0u; }

    const FastValues* fast_ = nullptr;
    IHostTransport& transport_;
    Presets*        presets_ = nullptr;
    Info            info_;

    const uint8_t* desc_     = nullptr;
    uint32_t       desc_len_ = 0u;
    uint32_t       desc_crc_ = 0u;
    bool           desc_read_valid_ = true;
    uint8_t        uid_[12]  = {};

    FrameParser parser_;
    uint32_t    read_generation_ = 0u;
    uint8_t     rx_buf_[128];              /* transport drain scratch  */
    uint8_t     dec_ [kMaxDecoded];        /* response, decoded        */
    uint8_t     wire_[kMaxWire];           /* response, COBS-encoded   */

    /* Host→device staged blob (BLOB_BEGIN/DATA/COMMIT); caller-owned. */
    uint8_t* staging_ = nullptr;
    size_t   buf_cap_ = 0u;
    bool     stage_open_    = false;
    uint8_t  stage_target_  = 0u;
    uint32_t stage_total_   = 0u;
    uint32_t stage_recv_    = 0u;
    uint32_t stage_schema_  = 0u;
    uint32_t stage_last_ms_ = 0u;

    /* Device→host live snapshot (GET_LIVE); caller-owned. */
    uint8_t* snapshot_ = nullptr;
    uint16_t snap_len_   = 0u;
    bool     snap_valid_ = false;

    /* Deferred reboot. */
    void (*reboot_fn_)(uint8_t, void*) = nullptr;
    void*    reboot_ctx_     = nullptr;
    bool     reboot_pending_ = false;
    uint8_t  reboot_mode_    = 0u;
    uint32_t reboot_at_ms_   = 0u;

    /* Registered command-block extensions (non-owning). */
    IHostlinkExtension* extensions_[kMaxExtensions] = {};
    uint8_t             num_extensions_             = 0u;
};

} // namespace hostlink
} // namespace alchemy
