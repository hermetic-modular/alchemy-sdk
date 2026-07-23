/**
 * @file alchemy/host_link/host_link.h
 * @brief HostLink — the module-side protocol engine (v1).
 *
 * Bridges a byte-stream transport (USB CDC in practice) to the Presets
 * store and the live managed state.  See docs/hostlink-protocol.md.
 *
 * Threading contract:
 *   - Poll() runs from the app's control loop (the 1 ms inner poll is
 *     ideal — each stop-and-wait round trip then costs ~1 ms of queuing).
 *     It pumps the transport, parses requests, and executes commands.
 *     All Presets/Serializable interaction happens here — the same
 *     thread that runs on-device save/load gestures, so host and panel
 *     operations serialize naturally.
 *   - The transport's receive side may run at interrupt level; it only
 *     fills its own ring buffer.
 *
 * HostLink never calls the audio callback and never writes engine state
 * directly.  It does, however, drive the SAME parameter producer the
 * front panel does: a LOAD_FROM_SLOT / live-audition applies a preset via
 * the app's normal per-frame "push params to engine" step.  Whatever
 * ISR/param synchronisation the app already relies on for on-panel knob
 * moves applies unchanged here — the only difference is that a host can
 * swap a whole parameter set at once (which a human turning one pot
 * cannot), so any pre-existing non-atomic param update becomes remotely,
 * and more sharply, inducible.  Apps that need glitch-free preset A/B
 * over the link should double-buffer their engine-param apply.
 */

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

    /** Attach the pre-rendered descriptor JSON (see DescriptorBuilder).
     *  Pass len = 0 to advertise "no descriptor".  The buffer must stay
     *  valid for the lifetime of the link. */
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

    IHostTransport& transport_;
    Presets&        presets_;
    Info            info_;

    const uint8_t* desc_     = nullptr;
    uint32_t       desc_len_ = 0u;
    uint32_t       desc_crc_ = 0u;
    uint8_t        uid_[12]  = {};

    FrameParser parser_;
    uint8_t     rx_buf_[128];              /* transport drain scratch  */
    uint8_t     dec_ [kMaxDecoded];        /* response, decoded        */
    uint8_t     wire_[kMaxWire];           /* response, COBS-encoded   */

    /* Host→device staged blob (BLOB_BEGIN/DATA/COMMIT); caller-owned. */
    uint8_t* staging_;
    size_t   buf_cap_;
    bool     stage_open_    = false;
    uint8_t  stage_target_  = 0u;
    uint32_t stage_total_   = 0u;
    uint32_t stage_recv_    = 0u;
    uint32_t stage_schema_  = 0u;
    uint32_t stage_last_ms_ = 0u;

    /* Device→host live snapshot (GET_LIVE); caller-owned. */
    uint8_t* snapshot_;
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
