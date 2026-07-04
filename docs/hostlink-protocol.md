# HostLink protocol v1

HostLink exposes a module's live state and preset slots to a host (website,
CLI) over a byte-stream transport — USB CDC serial in practice — while the
module runs normally.  It is a strict request/response protocol: the host
sends one frame and waits for the matching response before sending the next.

Design invariants:

- **Slots and blobs only.**  No raw flash addresses ever cross the wire.  The
  worst a hostile host can do is write a well-formed preset.
- **Firmware is strict.**  A blob whose schema hash or length does not match
  the *running* firmware is rejected at `BLOB_BEGIN`; migration is the host's
  job.  Flash never holds a blob the running firmware cannot load.
- **Live state is volatile.**  `BLOB_*` to target `live` auditions a state;
  nothing persists unless the host explicitly writes a slot or invokes
  `SAVE_TO_SLOT`.  This mirrors the on-device rule: settings persist only via
  explicit preset save.
- **Torn writes are safe.**  Slot writes go through the same ping-pong
  wear-levelled store as on-device saves; interrupting a write leaves the
  previous copy valid.

## 1. Transport and framing

Byte stream (USB CDC, 8N1-equivalent, baud ignored).  Frames are
[COBS](https://en.wikipedia.org/wiki/Consistent_Overhead_Byte_Stuffing)-encoded
and delimited by a single `0x00` byte after each frame.  A decoder scans to
`0x00`, COBS-decodes the bytes before it, and discards empty or undecodable
chunks (resync is automatic).

Decoded frame layout (all multi-byte fields little-endian):

| offset | size | field | notes |
|---|---|---|---|
| 0 | 1 | `proto` | `0x01` |
| 1 | 1 | `type`  | request `0x01..0x7F`; response = request `\| 0x80` |
| 2 | 2 | `seq`   | host-chosen; echoed verbatim in the response |
| 4 | 2 | `len`   | body length `N`, `0..512` |
| 6 | N | `body`  | |
| 6+N | 4 | `crc32` | over bytes `0 .. 6+N-1` (header + body) |

- CRC32: reflected, polynomial `0xEDB88320`, init `0xFFFFFFFF`, final XOR
  `0xFFFFFFFF` (same function the preset store uses).
- Max decoded frame = `6 + 512 + 4 = 522` bytes → max wire frame ≈ 526 bytes
  including COBS overhead and delimiter.
- A frame that fails CRC, has `proto != 1`, `len > 512`, or truncates is
  answered with an `ERR` frame (`type = 0xFF`) carrying `status =
  FRAME_ERROR` and the request's `seq` when recoverable (else `seq = 0`).
  Hosts match responses by `seq`; an unmatched `ERR` should be logged and
  otherwise ignored.

Every response body begins with a `u8 status`; remaining fields are present
only when `status == OK` unless noted.

Strings on the wire are `u8 length` followed by that many ASCII bytes (no
NUL).

## 2. Status codes

| value | name | meaning |
|---|---|---|
| 0 | `OK` | |
| 1 | `UNSUPPORTED` | unknown `type` |
| 2 | `BAD_ARGS` | malformed body, out-of-order offset, bad target |
| 3 | `BAD_STATE` | command illegal now (e.g. `BLOB_DATA` with no transaction) |
| 4 | `BAD_CRC` | `BLOB_COMMIT` payload CRC mismatch |
| 5 | `BAD_SLOT` | slot out of range, or load from invalid slot |
| 6 | `TOO_LARGE` | `total_len` exceeds capacity |
| 7 | `SCHEMA_MISMATCH` | blob schema/length differs from running firmware |
| 8 | `FLASH_FAIL` | erase/write failed |
| 9 | `BUSY` | transient; retry |
| 10 | `FRAME_ERROR` | transport-level frame reject (only in `ERR` frames) |

## 3. Commands

### 0x01 HELLO
Request body: empty.
Response body:

```
u8  status
u8  proto_ver        (1)
u8  board            (0 unknown, 1 = alchemy-lab v1, 2 = alchemy-lab v2)
u8  slot_count       (16)
u8  boot_slot        (0)
u8  mcu_uid[12]      (STM32 96-bit unique id)
u32 schema_hash      (combined hash of the running managed set)
u32 blob_capacity    (max payload bytes per slot)
u32 descriptor_len   (0 = descriptor unavailable)
u32 descriptor_crc32 (over the descriptor bytes)
u16 max_body         (512)
u16 live_size        (serialized size of the running managed set)
str module_id        ("echoa")
str module_name      ("Echoa")
str fw_version       (semver, e.g. "0.7.0")
str fw_git           (short hash)
str sdk_version
```

HELLO doubles as the identity probe: hosts open a port, send HELLO with a
short timeout, and treat no-answer as "not a HostLink device".  It is legal
at any time and never disturbs a transaction.

### 0x02 GET_DESCRIPTOR
Request: `u32 offset`, `u16 max_len`.
Response: `u8 status, u32 offset, u16 n, bytes[n]`.
Hosts fetch `descriptor_len` bytes in chunks; the concatenation is UTF-8 JSON
(§5) whose CRC32 must equal `descriptor_crc32`.

### 0x10 LIST_SLOTS
Request: empty.
Response: `u8 status, u8 count`, then `count` records of:

```
u8  flags        bit0 = CRC-valid record present
                 bit1 = stored schema matches running firmware
u32 seq          (wear-levelling sequence; 0 if invalid)
u16 length       (stored payload length; 0 if invalid)
u32 schema_hash  (stored; 0 if invalid)
```

### 0x11 READ_SLOT
Request: `u8 slot, u32 offset, u16 max_len`.
Response: `u8 status, u8 slot, u32 offset, u16 n, u16 total, u32 schema_hash,
bytes[n]`.
Reads the stored payload byte stream (the concatenated component bytes; the
schema hash and length ride in the header fields).  `n == 0` iff `offset >=
total`.  Readable regardless of schema match (backup of foreign-schema slots
is allowed); `BAD_SLOT` if no valid record.

### 0x12 BLOB_BEGIN
Request: `u8 target, u32 total_len, u32 schema_hash`.
`target` = `0..slot_count-1` for a slot write, `0xFF` for live audition.
Firmware requires `schema_hash == running schema_hash` **and** `total_len ==
live_size` (else `SCHEMA_MISMATCH`).  A `BLOB_BEGIN` while a transaction is
open aborts the old transaction and starts fresh.
Response: `u8 status`.

### 0x13 BLOB_DATA
Request: `u32 offset`, then payload bytes (rest of body).
`offset` must equal the byte count received so far (strictly in-order;
`BAD_ARGS` otherwise, which also aborts the transaction).
Response: `u8 status, u32 received`.

### 0x14 BLOB_COMMIT
Request: `u32 crc32` over the `total_len` staged bytes.
On match: target slot → assemble blob and write through the wear-levelled
store (same path as an on-device save, including watchdog-kicking yields);
target live → deserialize into the running components (same path as an
on-device load; pot catch re-arms).
Response: `u8 status` — sent **after** the flash write completes; hosts
should allow a 10 s timeout for slot commits.

A transaction with no `BLOB_DATA`/`BLOB_COMMIT` for 5 s is aborted
(subsequent `BLOB_DATA` → `BAD_STATE`).

### 0x15 ERASE_SLOT
Request: `u8 slot`.  Invalidates both ping-pong sides (header sector erase).
Response: `u8 status`.

### 0x20 GET_LIVE
Request: `u32 offset, u16 max_len`.
`offset == 0` serializes the running managed set into a snapshot buffer;
subsequent offsets read that snapshot (consistent multi-chunk reads).
Response: `u8 status, u8 target (0xFF), u32 offset, u16 n, u16 total,
u32 schema_hash, bytes[n]`.

### 0x30 SAVE_TO_SLOT
Request: `u8 slot`.  Exactly the on-device save gesture: serialize live state
and write the slot.  Response after completion (10 s timeout).

### 0x31 LOAD_FROM_SLOT
Request: `u8 slot`.  Exactly the on-device load gesture.  `BAD_SLOT` if the
slot is invalid or schema-mismatched.

### 0x40 REBOOT
Request: `u8 mode` — `0` = normal reset, `1` = reset into the DFU bootloader
(hand-off to the firmware-update flow).  Response `OK` is sent first; the
device resets ~100 ms later.

## 4. Concurrency model

All command execution happens in the module's control-loop context, never in
the audio interrupt, so host commands serialize naturally with on-device
gestures (a user saving from the panel and a host writing a slot cannot
interleave mid-operation).  Slot writes reuse the store's cooperative yield —
the watchdog is kicked and time-critical CV polling continues between flash
chunks, identical to on-device saves.

## 5. Descriptor JSON

The descriptor makes the module self-describing: it tells the host how to
decode, display, edit, and re-encode the blob byte stream.  It is rendered
once at boot (after controls are declared and defaults seeded, before the
boot preset loads — so `def` values are factory defaults).

```jsonc
{
  "dv": 1,                      // descriptor format version
  "module": {
    "id": "echoa", "name": "Echoa",
    "fw": "0.7.0", "git": "abc1234", "sdk": "1.0.0-beta",
    "board": "v1"
  },
  "schemaHash": 3735928559,     // combined hash, unsigned decimal
  "size": 170,                  // total blob payload bytes
  "components": [               // in Manage() order; offsets are blob-relative
    {
      "id": "perf", "kind": "pager",
      "hash": 123456, "size": 72, "off": 0,
      "pages": 3, "pots": 6,
      "pageNames": ["Time", "Space", "Engine"],    // optional page tab labels
      "pageColors": ["#86efac", "#fca5a5", "#67e8f9"],  // optional tab tints
      "fields": [               // one per (page,pot); off relative to component
        { "id": "d1.time", "name": "Time 1", "off": 0, "type": "f32",
          "page": 0, "pot": 0, "def": 0.61,
          "disp": { "kind": "expTime", "minS": 0.001,
                     "maxFrom": "global.timeMax", "maxMap": [2,4,8,12,16],
                     "curveFrom": "global.timeCurve" } }
        // ...
      ],
      "alt": {                  // optional alternate position→field mapping
        "layoutFrom": "global.layout",   // field id; zone 1 selects this map
        "pages": [ ["d1.time","d1.dif","d1.fb","d1.bloom","d1.mix","d1.damp"],
                    ["d2.time","d2.dif","d2.fb","d2.bloom","d2.mix","d2.damp"],
                    ["e1.morph","e2.morph","e1.depth","e2.depth","mt1.mode","mt2.mode"] ]
      }
    },
    { "id": "motion", "kind": "opaque", "hash": 456, "size": 640, "off": 72,
      "name": "Motion recording" },
    {
      "id": "settings", "kind": "settings", "hash": 789, "size": 27, "off": 712,
      "fields": [
        { "id": "routing.mode", "name": "Delay Mode", "off": 0, "type": "enum",
          "zones": 4, "page": 0, "pot": 0, "def": 0,
          "disp": { "kind": "enum",
                     "labels": ["Series Stereo","Parallel Stereo",
                                "Isolated Mono","Spectral Stereo"] } }
        // ...
      ]
    }
  ]
}
```

Field `type`s:

- `"f32"` — 4-byte LE float, natural range `[0,1]` (pot-normalized).
- `"enum"` — 1-byte zone index.

`disp` display hints (host-side rendering guidance; hosts must tolerate
unknown kinds and fall back to a raw percent readout):

| kind | extra keys | meaning |
|---|---|---|
| `norm` | — | percent 0–100 |
| `percent` | `scale` | value × scale, "%" |
| `bipolar` | — | float field holding `(v+1)/2`; show −100…+100 % |
| `brightness` | `lo`, `hi` | logical = lo + (hi−lo)·norm |
| `gain` | — | linear gain; show dB = 20·log₁₀(v) |
| `expTime` | `minS`, `maxFrom`, `maxMap`, `curveFrom` | delay seconds = minS·(max/minS)^v (or linear when the `curveFrom` enum field selects zone 1); `max` = `maxMap[zone of maxFrom field]` |
| `snap` | `labels[n]` | f32 snapped to n positions at i/(n−1) |
| `enum` | `labels[n]` | labels for enum zones |
| `linear` | `lo`, `hi`, `unit?` | value = lo + (hi−lo)·norm, shown with `unit` |
| `exp` | `lo`, `hi`, `unit?` | value = lo·(hi/lo)^norm (lo > 0), shown with `unit` |
| `cvDest` | `specials[5]` | zones 0–4 = specials; zone 5+k targets perf position (page = k / pots, pot = k % pots); label via the pager mapping under the active layout |

`pageNames` / `pageColors` are optional on `pager` and `settings`
components: arrays of exactly `pages` entries labeling and tinting the
host's page tabs.  An empty-string entry means "no label for this page" —
the host falls back to a generic label (colors likewise).  Colors are CSS
hex strings; hosts must validate before use and ignore malformed values.

Component `kind`s: `pager` and `settings` are field-bearing; `opaque`
round-trips byte-exact and is shown as a summary chip (e.g. "Motion
recording · clear"); `name` is a fixed NUL-padded UTF-8 buffer holding
the preset's display name — hosts read it and may rewrite it in place
(truncate to `size − 1` bytes without splitting a multi-byte sequence;
the final byte is always NUL).

Any other kind (the SDK emits `fields` for custom components) is
field-bearing when it carries a `fields` array — hosts render it as a
plain editable group labeled by the optional `name` key; its fields omit
`page`/`pot`.  Unknown kinds **without** fields must be treated as
opaque.

### 5.1 Auto-description

Firmware built on the SDK's surface stack does not hand-write this JSON:
`hostlink::Host` derives it from the managed components at boot — knob
names/transforms become fields and display hints, `Page::Name()/Color()`
become `pageNames`/`pageColors`, Settings introspection yields the
settings component, and custom `Serializable`s describe themselves via
`Describe(ComponentWriter&)` (or fall back to `opaque`).  Field ids
default to positional (`p<page>.<pot>`, `s<page>.<pot>`) unless pinned
with `.Ident()`.  Hand-rolled descriptors (this section's JSON, emitted
via `DescriptorBuilder`) remain fully supported and byte-compatible.

Cross-field references (`maxFrom`, `curveFrom`, `layoutFrom`,
`alt.layoutFrom`) name field ids **within the same blob**; hosts resolve them
against current (edited) values so displays stay live during editing.

## 6. Versioning rules

- **Protocol**: breaking wire changes bump `proto` and HELLO's `proto_ver`.
  New commands / trailing response fields are non-breaking (hosts ignore
  extras; firmware answers `UNSUPPORTED` for unknown types).
- **Descriptor**: additive keys are non-breaking; structural changes bump `dv`.
- **Schema**: `schemaHash` governs blob compatibility exactly as it does for
  on-device loads.  Hosts migrate old exports by re-encoding field values
  (matched by stable field `id`) under the new descriptor, defaulting new
  fields and dropping removed ones; `opaque` components migrate only when
  their individual `hash` is unchanged.

## 7. Host implementation notes

- Stop-and-wait: one in-flight request.  Suggested timeouts: 2 s default,
  10 s for `BLOB_COMMIT`/`SAVE_TO_SLOT`.  One retry on timeout is safe for
  every command except `BLOB_DATA` (whose retry would desync `offset` —
  instead restart the transaction).
- Chunk size: `max_body` from HELLO governs `BLOB_DATA` payloads
  (`max_body − 4`) and sensible `max_len` values (≤ `max_body − 16`).
- WebSerial exposes only VID/PID (`0x0483:0x5740`, the libDaisy CDC
  identity, shared with other Daisy devices) — identify by HELLO, not by
  USB descriptor.
