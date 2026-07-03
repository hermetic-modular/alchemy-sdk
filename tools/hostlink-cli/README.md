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
