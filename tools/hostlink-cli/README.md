# hostlink-cli

Dependency-free bench tool for driving a HostLink-capable module over USB
CDC from a terminal. Same wire protocol as the firmware and website
(`docs/hostlink-protocol.md`); its codecs are pinned to the SDK golden
vectors (`node hostlink.mjs selftest`).

macOS / Linux only (uses `stty` + `/dev` device nodes).

## Usage

```sh
# Handshake / identity
./hostlink.mjs -p /dev/tty.usbmodem1234 hello

# Slot table
./hostlink.mjs -p <port> list

# Descriptor JSON
./hostlink.mjs -p <port> descriptor > echoa.descriptor.json

# Back up / restore a single slot (raw blob)
./hostlink.mjs -p <port> read 3  > slot3.bin
./hostlink.mjs -p <port> write 3 slot3.bin

# Live state (audition is volatile — never persists)
./hostlink.mjs -p <port> getlive > live.bin
./hostlink.mjs -p <port> setlive live.bin

# On-device gestures, remotely
./hostlink.mjs -p <port> save 5    # SAVE_TO_SLOT
./hostlink.mjs -p <port> load 5    # LOAD_FROM_SLOT
./hostlink.mjs -p <port> erase 5

# Hand off to the firmware updater
./hostlink.mjs -p <port> reboot bootloader
```

Slot numbers are 1-based on the command line (slot 1 = the boot preset).
With no `-p/--port`, the first `/dev/tty.usbmodem*` (macOS) or
`/dev/ttyACM*` (Linux) is used.

## Selftest (no hardware)

```sh
node hostlink.mjs selftest
```

Validates COBS / CRC32 / frame codecs against
`../../tests/host/golden/hostlink_golden.json` — the same vectors the
website's TypeScript suite checks, so all three implementations agree on
the bytes.


## Device diagnostics

```sh
./hostlink.mjs -p <port> logs                  # retained messages, then exit
./hostlink.mjs -p <port> logs --follow         # continue until Ctrl-C
./hostlink.mjs -p <port> logs --level warn      # warnings and errors
./hostlink.mjs -p <port> logs --json           # JSON Lines, including gap notices
./hostlink.mjs -p <port> watch                 # live typed values
./hostlink.mjs -p <port> watch --json
```

Firmware must opt into `hostlink::Diagnostics`; see
[USB diagnostics](../../docs/diagnostics.md). Close the browser's device
connection before using the CLI. This is a HostLink client, not a raw text
terminal. Reads are non-destructive and reconnects can recover retained
history. A disconnected port ends the command; reconnect with the new
port name if the OS changed it. In-band diagnostic session resets are
rediscovered automatically.

Diagnostics codec tests consume the C++ generated wire vectors:

```sh
node --test tools/hostlink-cli/diagnostics.test.mjs # from SDK root
```

The transport-neutral `diagnostics-codec.mjs` mirrors the programmer's
`src/lib/settings-link/diagnostics.ts`; update the codec and shared golden
fixtures together when changing the protocol. No npm dependencies are
required to run this CLI.
