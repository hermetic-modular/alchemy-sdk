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
| 4 | 2 | `len`   | body length `N`, `0..max_body` |
| 6 | N | `body`  | |
| 6+N | 4 | `crc32` | over bytes `0 .. 6+N-1` (header + body) |

- CRC32: reflected, polynomial `0xEDB88320`, init `0xFFFFFFFF`, final XOR
  `0xFFFFFFFF` (same function the preset store uses).
- `max_body` is a per-module build constant announced in HELLO — `512` by
  default, up to `2048` on storage-capable modules.  Hosts must not send a
  body larger than the announced value; the HELLO exchange itself always
  fits the 512-byte minimum, so discovery is never a chicken-and-egg
  problem.  Max decoded frame = `6 + max_body + 4` bytes → max wire frame
  is that plus COBS overhead (1 byte per 254) and the delimiter.
- A frame that fails CRC, has `proto != 1`, `len > max_body`, or truncates
  is answered with an `ERR` frame (`type = 0xFF`) carrying `status =
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
| 11 | `FS_NO_CARD` | no SD card present / volume unmountable (§8) |
| 12 | `FS_NO_FILE` | path does not exist |
| 13 | `FS_EXISTS` | target exists and `overwrite` not set |
| 14 | `FS_LOCKED` | file is in use by the firmware (e.g. active recording) |
| 15 | `FS_FULL` | volume out of space |
| 16 | `FS_IO` | filesystem / card I/O error |

Hosts must tolerate status values they do not recognize (render a generic
"device error (N)"), since new commands may introduce new codes.

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

### 0x50–0x5F Filesystem block
Optional SD-card access — commands, invariants, and host guidance in §8.

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
plain editable group labeled by the optional `name` key.  Fields of
*generic* kinds omit `page`/`pot`; a defined kind may specify its own
field keys (`buttons` fields carry `page`, §5.5).  Unknown kinds
**without** fields must be treated as opaque.

Custom kinds may also emit per-kind metadata keys (e.g. `slots`,
`slotSize`, `layout`) between the header and the `fields` array — hosts
that recognize the kind consume them, others ignore them and fall back
to the generic field-bearing / opaque rendering.  The `param_locks`
kind (§5.4) is one example.

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

### 5.2 Optional root keys

Beyond `buttons` (§5.3), a module with SD storage adds a `storage` root
key (see §8) so hosts can light up file-management UI without probing:

```jsonc
"storage": { "sd": true, "fsv": 1 }
```

Unknown root keys must be ignored (additive, per §6).

### 5.3 Buttons (root metadata array — legacy)

A module may declare its physical push buttons at the descriptor root as
a `buttons` array (adjacent to `components`).  This is the original,
metadata-only form; modules built on the `ButtonBank` surface emit a
`buttons` *component* instead (§5.5), which additionally carries the
buttons' persisted state per page.  A module uses one form or the other,
and hosts must keep supporting both.  Root-array buttons are neither
addressable nor mutable through the wire protocol.
Their purpose is to let the host render meaningful chips ("what does B2
do?  what's it currently set to?") and, for state-controlling buttons,
back-reference the preset field(s) the button mutates so the current
value can be shown next to the button and read back consistently.

```jsonc
"buttons": [
  { "id": "b1", "name": "Page / Lock", "role": "modal",
    "actions": [
      { "gesture": "tap",       "label": "Next Page" },
      { "gesture": "hold+knob", "label": "Record Param Lock" }
    ]
  },
  { "id": "b2", "name": "Osc Source", "role": "state",
    "actions": [ { "gesture": "tap", "label": "Cycle Int / Ext / Both" } ],
    "controls": [ { "field": "source", "action": "cycle" } ]
  },
  { "id": "b3", "name": "Sub", "role": "state",
    "actions": [
      { "gesture": "tap",  "label": "Cycle Octave −1 / −2 / −3" },
      { "gesture": "hold", "label": "Toggle Square / Sine" }
    ],
    "controls": [
      { "field": "sub.octave", "action": "cycle"  },
      { "field": "sub.wave",   "action": "toggle" }
    ]
  }
]
```

- `id` — stable identifier (hosts key on this; never rename).
- `role` — `"modal"` (transient gestures — page advance, lock record —
  no persistent preset relationship) or `"state"` (each press mutates
  one or more preset fields).  Unknown roles must be shown as a plain
  chip without control affordances.
- `actions[]` — labeled gestures (`tap`, `hold`, `hold+knob`,
  `double_tap`, …); `gesture` values are strings so hosts can
  forward-tolerate new ones.
- `controls[]` (state buttons only) — each entry names a preset field
  by id and the mutation kind (`cycle`, `toggle`, `set`, …).  The named
  field lives in a normal component; the button entry does not carry
  the field's value — the host reads it via the usual blob decode.

The array is optional; a module that omits it is rendered without a
button panel.

### 5.4 Parameter locks (`kind: "param_locks"`)

A `param_locks` component wraps a slot-array of looping-automation
recordings that live in the preset blob.  The component is opaque at
the field level — locks are recorded on-device by hardware gesture, not
edited from the host — but the descriptor exposes the per-slot layout
so hosts can *view* the recorded envelopes and *delete* individual
slots by zeroing their `active` byte.

```jsonc
{ "id": "locks", "kind": "param_locks",
  "hash": 0xDEADBEEF, "size": 12372, "off": 96,
  "name": "Param Locks",
  "slots": 12, "slotSize": 1031, "bufLen": 256,
  "layout": {
    "activeOff": 0, "lengthOff": 1, "baseOff": 3, "bufOff": 7
  }
}
```

- `slots` — number of automation slots in this component.
- `slotSize` — bytes per slot; `size` == `slots * slotSize`.
- `bufLen` — envelope buffer length in f32 samples (protocol constant;
  redundantly exposed so hosts don't have to encode it).
- `layout.*Off` — byte offsets within one slot for the `active`
  (u8), `length` (u16, LE), `record_base` (f32, LE), and buffer (f32
  array, LE) fields.

To render slot `i`:

1. Read `blob[c.off + i*slotSize + activeOff]` — skip if zero.
2. Read `length` (u16) at `bufOff + lengthOff` within the slot.
3. Read `record_base` (f32) at `bufOff + baseOff`.
4. Read `length` f32 samples starting at `bufOff + bufOff` — the y-axis
   is the recorded normalized value; plot at unit spacing.

To delete a slot: write `0` to the slot's `activeOff` byte in the blob
and push via `SET_LIVE`.  Save-to-slot to persist.

The kind is field-less on purpose: the fields path is for
knob-shaped edits, and per-lock automation is not that.  Hosts that do
not recognize `param_locks` must fall back to the generic no-fields
rendering (an unnamed chip) — the schema hash still guarantees
byte-exact round-trip.

### 5.5 Buttons component (`kind: "buttons"`)

A module built on the `ButtonBank` surface emits its buttons as one
component.  Stateful buttons are **ordinary editable enum fields** — one
byte each, dense, in roster order — so a host that has never heard of
this kind still renders them as a plain editable group and can read,
edit, and write them through the normal blob paths.  A button-aware host
uses the extra keys to draw each button inside its page card instead.

```jsonc
{ "id": "buttons", "kind": "buttons", "hash": 2864434397,
  "size": 2, "off": 96,
  "modal": [                       // momentary buttons: metadata only
    { "id": "trig", "name": "Trigger", "index": 1, "page": 0,
      "gestures": [ { "gesture": "tap", "label": "Fire the kick" } ] }
  ],
  "fields": [
    { "id": "flt.mode", "name": "Filter", "off": 0, "page": 0,
      "type": "enum", "zones": 3, "def": 0,
      "disp": { "kind": "enum", "labels": ["LP", "BP", "HP"] },
      "anchor": "flt.cutoff",
      "btn": { "index": 2, "action": "cycle",
               "gestures": [ { "gesture": "tap",
                               "label": "Cycle LP / BP / HP" } ] } },
    { "id": "osc.sub", "name": "Sub", "off": 1, "page": 1,
      "type": "enum", "zones": 2, "def": 0,
      "disp": { "kind": "enum" },
      "btn": { "index": 2, "action": "toggle",
               "gestures": [ { "gesture": "tap",
                               "label": "Toggle" } ] } }
  ]
}
```

- `page` (fields and modal entries) — the page the button lives on.
  Omitted when the button appears on several pages or is global:
  render it on every page.
- `anchor` — optional attachment: render this control with the named
  field (typically its companion knob) wherever the host renders that
  field; where the target is not shown, fall back to the page's plain
  button group.  Targets are field ids from any component (never
  `modal` ids); firmware validates the ref at build time, so a host
  never sees a dangling one — but must still tolerate junk per §6.
  May appear on fields with no `btn` (host-only state).
- `btn` — the physical binding: `index` is the hardware button,
  `action` the primary tap mutation (`cycle` / `toggle` / `set`), and
  `gestures[]` labeled gestures exactly as in §5.3.  A field without
  `btn` is host-editable state with no panel gesture.
- `modal[]` — buttons with no persisted state (page cyclers, triggers,
  lock recorders).  Metadata only; contributes no blob bytes.  Must
  precede `fields` when both are present.

The same physical `index` may appear in several entries with different
`page` values — the same button does different things on different
pages.  Hosts that do not recognize the kind must ignore `modal`,
`anchor`, and `btn` per §6 and fall back to the generic field-bearing
rendering; editing still works, which is the point.

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

## 8. Filesystem access (commands 0x50–0x5F)

Storage-capable modules expose their SD card's FAT volume through the
`0x50` command block.  The block is optional: modules without storage
answer `UNSUPPORTED`, and capable modules additionally advertise it in
the descriptor root:

```jsonc
"storage": { "sd": true, "fsv": 1 }
```

`fsv` versions the filesystem command set itself.  Hosts should treat the
descriptor key as the fast path and an `FS_INFO` probe as the fallback.

Design invariants, extending §"Design invariants":

- **Path-jailed.**  All paths are absolute within the SD volume; there is
  no way to address flash, RAM, or any other storage through this block.
- **Uploads are atomic.**  Host writes stage into `<path>.part` and only
  an explicitly requested, CRC-verified `FS_COMMIT` renames the staged
  file onto its final name.  A power cut or abandoned transfer can never
  leave a torn file under a final name.
- **Firmware-owned files are protected.**  A file the firmware holds open
  (e.g. an in-progress recording) is flagged `locked` in listings and
  refuses host writes, deletion, and renaming with `FS_LOCKED`.  Reads of
  locked files are allowed.
- **Nothing is deleted implicitly.**  Stale `.part`/`.partmeta` files
  from interrupted uploads stay on the card — visible, resumable, and
  deletable through this same interface — rather than being swept by
  hidden firmware policy.

### 8.1 Paths

- Absolute, `/`-separated, starting with `/` (the volume root).  The
  volume prefix (`0:`) never appears on the wire.
- Total path ≤ 192 bytes; each segment 1–64 bytes.
- Characters: printable ASCII `0x20..0x7E` excluding `\ : * ? " < > |`.
  Segments must not be `.` or `..`, must not end with a dot or space.
- `FS_OPEN_WRITE` targets must not end in `.part` or `.partmeta`
  (reserved staging suffixes) — `BAD_ARGS`.
- Violations answer `BAD_ARGS`.

### 8.2 Transfer model

At most **one file transfer** (read or write) is open at a time; opening
a new one implicitly aborts the previous.  A transfer with no traffic
for 5 s is aborted (subsequent `FS_READ`/`FS_WRITE` → `BAD_STATE`).  An
aborted or closed *upload* keeps its `.part`/`.partmeta` staging files
so it can be resumed (§8.4) or discarded by deleting them.

Directory listing uses an independent cursor (§8.3) and may interleave
with an open transfer.  The blob transaction (§3) is likewise
independent: preset traffic and file traffic never conflict.

All commands execute in the module's control-loop context (§4), so host
file operations serialize naturally with the firmware's own storage work
(e.g. an SD recorder) — but they contend for the same card bandwidth;
hosts should expect elevated latency variance while the module is
actively recording.

### 8.3 Commands

Timestamps are raw FAT format: `u32 mtime = (fdate << 16) | ftime`
(local device time; modules without a clock write the FAT epoch).  `0`
means unknown.  Entry `flags` bits: `bit0` directory, `bit1` read-only,
`bit2` locked by firmware.

#### 0x50 FS_INFO
Request: empty.  May trigger the deferred mount; allow a 10 s timeout.

An `FS_NO_CARD` response carries a diagnostic trailer so hosts can say
*why*, not just "no card":

```
u8 mount_fr        FatFS FRESULT of the failed mount (0xFE = untried)
u8 disk_init       diskio DSTATUS (0 = controller/card came up)
sector_probe s0    raw sector 0
u8 pt_types[4]     MBR partition type IDs from sector 0
u32 pt1_lba        partition 1 start LBA
sector_probe p1    partition 1's boot sector (probed only when sector 0
                   is an MBR with a nonzero first partition)

sector_probe = u8 read_result (diskio DRESULT; 0xFF = not attempted),
               u8 sig[2] (bytes 510..511), u8 first (byte 0),
               u8 oem[8] (bytes 3..10), u8 fstype[8] (bytes 82..89)
```

Evidence beats codes: `pt_types[0] = 0x07` + OEM "EXFAT   " means the
card is exFAT-formatted; zeroed signatures with `read_result = 0` mean
the data path returns blanks; a valid "FAT32   " fstype alongside a
mount failure points at the firmware.  Hosts must tolerate a shorter
trailer (older firmware).
Response:

```
u8  status        (FS_NO_CARD if absent/unmountable → diagnostic trailer)
u8  card_state    (0 = no card / unmounted, 1 = mounted)
u8  fs_type       (FatFS values: 2 = FAT16, 3 = FAT32; 0 = unknown)
u8  flags         (bit0 = free_kib valid, bit1 = firmware storage busy)
u32 total_kib
u32 free_kib      (0 when bit0 clear)
u16 max_name      (max path segment length, 64)
```

Free space derives from the volume's FSINFO sector; on cards where that
is unavailable the firmware reports `free_kib` invalid rather than
stalling to scan the FAT.

#### 0x51 FS_LIST
Request: `u32 cookie, str path`.  `cookie = 0` starts a listing of
`path`; continue by echoing the returned `next_cookie` with the same
path.  The firmware keeps one cursor; a stale or foreign cookie is
honored by transparently reopening the directory and skipping, so
listings are always completable (merely slower after eviction).
Response: `u8 status, u32 next_cookie (0xFFFFFFFF = end), u8 count`,
then `count` records of `u8 flags, u32 size, u32 mtime, str name`.
Records are packed to the body limit; order is the on-disk directory
order, stable while the directory is unmodified.

#### 0x52 FS_STAT
Request: `str path`.
Response: `u8 status, u8 flags, u32 size, u32 mtime`.

#### 0x53 FS_OPEN_READ
Request: `str path`.  Opens the read transfer (aborting any prior
transfer).  `BAD_ARGS` for directories.
Response: `u8 status, u32 size, u32 mtime`.

#### 0x54 FS_READ
Request: `u32 offset, u16 max_len`.
Response: `u8 status, u32 offset, u16 n, bytes[n]`.  `n == 0` iff
`offset ≥ size`.  Offsets are arbitrary (the transfer file is seekable)
and requests are idempotent — see §8.5 for pipelining.

#### 0x55 FS_CLOSE
Request: empty.  Closes any open transfer.  For an uncommitted upload
the staging files are kept (resumable); for reads it simply releases
the file.  Response: `u8 status` (`OK` even if nothing was open).

#### 0x56 FS_OPEN_WRITE
Request: `u8 flags, u32 total_len, str path`.
`flags`: `bit0` overwrite existing target, `bit1` create missing parent
directories, `bit2` resume.
Opens the write transfer, staging into `<path>.part`.  Without `resume`
any existing staging files are truncated.  With `resume`, a matching
`<path>.partmeta` (same `total_len`, consistent `.part` size) lets the
transfer continue: the response's `resume_offset` tells the host where
to continue `FS_WRITE`; on any mismatch the firmware restarts cleanly
and answers `resume_offset = 0`.  `FS_EXISTS` if the final target
exists and `overwrite` is clear.
Response: `u8 status, u32 resume_offset`.

#### 0x57 FS_WRITE
Request: `u32 offset`, then payload bytes (rest of body).  `offset`
must equal bytes staged so far (strictly in-order; `BAD_ARGS` aborts
the transfer's session state but keeps staging files for resume).  The
firmware maintains a running CRC32 of the staged stream and
periodically checkpoints `{received, crc_state}` into
`<path>.partmeta`, which is what makes resume after a power cut
possible.
Response: `u8 status, u32 received`.

#### 0x58 FS_COMMIT
Request: `u32 crc32` over all `total_len` bytes.
Requires the staged byte count to equal `total_len` (`BAD_STATE`
otherwise).  On CRC match: staging files are finalized — `.partmeta`
deleted, `.part` renamed onto the final path (unlinking an existing
target only when `overwrite` was set; a target that appeared since
`FS_OPEN_WRITE` without `overwrite` fails `FS_EXISTS`).  `BAD_CRC`
keeps the staging files (host may re-verify / resume).
Response: `u8 status`, sent after the rename completes (10 s timeout).

#### 0x59 FS_DELETE
Request: `str path`.  Files and *empty* directories; deleting a
non-empty directory answers `BAD_STATE` (hosts recurse leaf-first).
`FS_LOCKED` for firmware-held files.
Response: `u8 status`.

#### 0x5A FS_MKDIR
Request: `str path`.  Parents must exist (`FS_NO_FILE` otherwise);
`FS_EXISTS` if the path already exists.
Response: `u8 status`.

#### 0x5B FS_RENAME
Request: `str from, str to`.  Same volume; moves across directories are
allowed; existing targets are not overwritten (`FS_EXISTS`).
`FS_LOCKED` for firmware-held sources.
Response: `u8 status`.

### 8.4 Crash safety and resume

The staging pair written during an upload:

- `<path>.part` — the staged bytes, append-only.
- `<path>.partmeta` — 16 bytes, rewritten (and synced) every 1 MiB of
  staged data: `u32 magic "HMPM", u32 total_len, u32 received,
  u32 crc_state` (the CRC32 of the first `received` staged bytes —
  the protocol CRC32 is chainable, so this doubles as the resume seed).

A power cut mid-upload therefore loses at most the un-checkpointed tail;
`FS_OPEN_WRITE(resume)` re-anchors at the last checkpoint (truncating
any `.part` bytes beyond it) and the host resumes from `resume_offset`.
The final path only ever appears via the commit-time rename, and
`FS_COMMIT`'s CRC check covers the resumed whole, not just the tail.

### 8.5 Host implementation notes

- Stop-and-wait applies (§7), with one sanctioned exception: within an
  open read transfer, hosts MAY keep up to **3** `FS_READ` requests in
  flight (distinct `seq`, disjoint offsets).  Reads are idempotent and
  the firmware executes frames in arrival order, so responses arrive in
  order; a lost frame is recovered by re-requesting that offset.  Do
  not pipeline any other command.
- Suggested timeouts: 2 s default; 10 s for `FS_INFO`, `FS_OPEN_READ`,
  `FS_OPEN_WRITE`, `FS_COMMIT` (deferred mount, card housekeeping).
- Chunking: `FS_READ max_len ≤ max_body − 7`; `FS_WRITE` payload
  ≤ `max_body − 4`.
- Feature detection: descriptor `storage` key, else probe `FS_INFO`
  and treat `UNSUPPORTED` as "no filesystem".
- Throughput expectation: bounded by USB Full-Speed CDC and `max_body`
  — order 250 KB/s at 512-byte bodies, approaching 1 MB/s with
  2048-byte bodies plus read pipelining.  Surface rates and ETAs in UI
  for anything beyond a few MB.
- FAT limits surface as protocol errors, not surprises: FAT32-formatted
  cards only (`FS_NO_CARD` on exFAT), 4 GiB per file, ASCII names
  (firmware-side validation; hosts should sanitize upload names and
  say so).
