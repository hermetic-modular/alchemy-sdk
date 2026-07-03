#!/usr/bin/env node
/**
 * hostlink.mjs — dependency-free HostLink bench CLI.
 *
 * Speaks the exact wire protocol the firmware and website use (see
 * docs/hostlink-protocol.md), for driving real hardware from a terminal
 * before any UI work is in play.  No npm dependencies: the serial port
 * is put in raw mode with `stty`, then read/written as a plain fd.
 *
 * macOS / Linux only (uses stty + /dev device nodes).
 *
 *   ./hostlink.mjs --port /dev/tty.usbmodem1234 hello
 *   ./hostlink.mjs -p /dev/ttyACM0 list
 *   ./hostlink.mjs -p <port> descriptor > echoa.descriptor.json
 *   ./hostlink.mjs -p <port> read 3 > slot3.bin
 *   ./hostlink.mjs -p <port> write 3 slot3.bin
 *   ./hostlink.mjs -p <port> getlive | xxd | head
 *   ./hostlink.mjs -p <port> save 5
 *   ./hostlink.mjs -p <port> load 5
 *   ./hostlink.mjs -p <port> erase 5
 *   ./hostlink.mjs -p <port> reboot bootloader
 *
 * With no --port, the first /dev/tty.usbmodem* (macOS) or /dev/ttyACM*
 * (Linux) is used.
 */

import { execFileSync } from "node:child_process";
import { readdirSync, readFileSync, writeFileSync, openSync, readSync, writeSync, closeSync } from "node:fs";

/* ── Wire codecs (mirror alchemy/host_link/*.h; pinned by golden tests) ── */

const PROTO = 1;
const MAX_BODY = 512;
const RESP_FLAG = 0x80;
const ERR_TYPE = 0xff;
const TARGET_LIVE = 0xff;

const CMD = {
  hello: 0x01,
  getDescriptor: 0x02,
  listSlots: 0x10,
  readSlot: 0x11,
  blobBegin: 0x12,
  blobData: 0x13,
  blobCommit: 0x14,
  eraseSlot: 0x15,
  getLive: 0x20,
  saveToSlot: 0x30,
  loadFromSlot: 0x31,
  reboot: 0x40,
};

const STATUS = [
  "ok", "unsupported", "bad-args", "bad-state", "bad-crc", "bad-slot",
  "too-large", "schema-mismatch", "flash-fail", "busy", "frame-error",
];

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[i] = c >>> 0;
  }
  return t;
})();

function crc32(buf, seed = 0) {
  let crc = ~seed >>> 0;
  for (let i = 0; i < buf.length; i++) crc = (CRC_TABLE[(crc ^ buf[i]) & 0xff] ^ (crc >>> 8)) >>> 0;
  return ~crc >>> 0;
}

function cobsEncode(input) {
  const out = Buffer.alloc(input.length + Math.ceil(input.length / 254) + 1);
  let o = 0, codeAt = o++, code = 1;
  for (let i = 0; i < input.length; i++) {
    if (input[i] === 0) { out[codeAt] = code; codeAt = o++; code = 1; }
    else {
      out[o++] = input[i];
      if (++code === 0xff) { out[codeAt] = code; codeAt = o++; code = 1; }
    }
  }
  out[codeAt] = code;
  return out.subarray(0, o);
}

function cobsDecode(input) {
  if (input.length === 0) return null;
  const out = Buffer.alloc(input.length);
  let o = 0, i = 0;
  while (i < input.length) {
    const code = input[i++];
    if (code === 0) return null;
    const run = code - 1;
    if (i + run > input.length) return null;
    for (let k = 0; k < run; k++) { const b = input[i++]; if (b === 0) return null; out[o++] = b; }
    if (code !== 0xff && i < input.length) out[o++] = 0;
  }
  return out.subarray(0, o);
}

function buildFrame(type, seq, body) {
  const dec = Buffer.alloc(6 + body.length + 4);
  dec[0] = PROTO;
  dec[1] = type;
  dec.writeUInt16LE(seq, 2);
  dec.writeUInt16LE(body.length, 4);
  body.copy(dec, 6);
  dec.writeUInt32LE(crc32(dec.subarray(0, 6 + body.length)), 6 + body.length);
  const enc = cobsEncode(dec);
  return Buffer.concat([enc, Buffer.from([0])]);
}

class FrameParser {
  constructor() { this.acc = []; }
  *push(byte) {
    if (byte !== 0) { this.acc.push(byte); return; }
    const chunk = Buffer.from(this.acc);
    this.acc = [];
    if (chunk.length === 0) return;
    const dec = cobsDecode(chunk);
    if (!dec || dec.length < 10) return;
    const bodyLen = dec.length - 10;
    const ok =
      dec[0] === PROTO &&
      dec.readUInt16LE(4) === bodyLen &&
      crc32(dec.subarray(0, dec.length - 4)) === dec.readUInt32LE(dec.length - 4);
    yield {
      type: dec[1],
      seq: dec.readUInt16LE(2),
      body: dec.subarray(6, 6 + bodyLen),
      ok,
    };
  }
}

/* ── Raw serial transport (stty + fd) ──────────────────────────────── */

function defaultPort() {
  const dev = "/dev/";
  const names = readdirSync(dev).filter(
    (n) => n.startsWith("tty.usbmodem") || n.startsWith("cu.usbmodem") || n.startsWith("ttyACM"),
  );
  if (names.length === 0) throw new Error("no usbmodem/ttyACM device found; pass --port");
  return dev + names.sort()[0];
}

class SerialLink {
  constructor(port) {
    this.port = port;
    try {
      execFileSync("stty", ["-f", port, "raw", "-echo", "115200"], { stdio: "ignore" });
    } catch {
      // Linux stty uses -F; try that spelling.
      execFileSync("stty", ["-F", port, "raw", "-echo", "115200"], { stdio: "ignore" });
    }
    this.fd = openSync(port, "r+");
    this.parser = new FrameParser();
    this.seq = 1;
    this.buf = Buffer.alloc(4096);
  }

  close() { try { closeSync(this.fd); } catch { /* ignore */ } }

  /** Send a command and await its matching response (or ERR). */
  request(type, body = Buffer.alloc(0), timeoutMs = 3000) {
    const seq = this.seq;
    this.seq = (this.seq + 1) & 0xffff || 1;
    writeSync(this.fd, buildFrame(type, seq, body));

    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      let n = 0;
      try { n = readSync(this.fd, this.buf, 0, this.buf.length, null); } catch { n = 0; }
      for (let i = 0; i < n; i++) {
        for (const f of this.parser.push(this.buf[i])) {
          if (f.seq !== seq) continue;
          if (f.type === ERR_TYPE) throw new Error(`frame error: ${STATUS[f.body[0]] ?? f.body[0]}`);
          if (f.type !== (type | RESP_FLAG)) throw new Error(`unexpected type 0x${f.type.toString(16)}`);
          const status = f.body[0];
          if (status !== 0) throw new Error(`device status: ${STATUS[status] ?? status}`);
          return f.body;
        }
      }
    }
    throw new Error(`timeout waiting for response to 0x${type.toString(16)}`);
  }
}

/* ── High-level ops ─────────────────────────────────────────────────── */

function readStr(buf, at) {
  const n = buf[at];
  return [buf.subarray(at + 1, at + 1 + n).toString("ascii"), at + 1 + n];
}

function hello(link) {
  const b = link.request(CMD.hello);
  let at = 5;
  const uid = b.subarray(at, at + 12).toString("hex"); at += 12;
  const schema = b.readUInt32LE(at); at += 4;
  const cap = b.readUInt32LE(at); at += 4;
  const dlen = b.readUInt32LE(at); at += 4;
  const dcrc = b.readUInt32LE(at); at += 4;
  const maxBody = b.readUInt16LE(at); at += 2;
  const liveSize = b.readUInt16LE(at); at += 2;
  let id, name, fw, git, sdk;
  [id, at] = readStr(b, at);
  [name, at] = readStr(b, at);
  [fw, at] = readStr(b, at);
  [git, at] = readStr(b, at);
  [sdk, at] = readStr(b, at);
  return {
    proto: b[1], board: b[2], slots: b[3], bootSlot: b[4], uid,
    schemaHash: schema >>> 0, blobCapacity: cap, descriptorLen: dlen, descriptorCrc: dcrc >>> 0,
    maxBody, liveSize, id, name, fw, git, sdk,
  };
}

function getDescriptor(link, info) {
  if (info.descriptorLen === 0) return "";
  const chunkMax = info.maxBody - 7;
  const parts = [];
  let off = 0;
  while (off < info.descriptorLen) {
    const req = Buffer.alloc(6);
    req.writeUInt32LE(off, 0);
    req.writeUInt16LE(chunkMax, 4);
    const b = link.request(CMD.getDescriptor, req);
    const n = b.readUInt16LE(5);
    if (n === 0) break;
    parts.push(Buffer.from(b.subarray(7, 7 + n)));
    off += n;
  }
  const all = Buffer.concat(parts);
  if (crc32(all) !== info.descriptorCrc) throw new Error("descriptor CRC mismatch");
  return all.toString("utf8");
}

function listSlots(link) {
  const b = link.request(CMD.listSlots);
  const count = b[1];
  const out = [];
  for (let s = 0; s < count; s++) {
    const at = 2 + s * 11;
    out.push({
      slot: s,
      valid: (b[at] & 1) !== 0,
      schemaMatch: (b[at] & 2) !== 0,
      seq: b.readUInt32LE(at + 1),
      length: b.readUInt16LE(at + 5),
      schemaHash: b.readUInt32LE(at + 7) >>> 0,
    });
  }
  return out;
}

function readStream(link, mkReq, maxBody) {
  const chunkMax = maxBody - 14;
  const first = mkReq(0, chunkMax);
  const total = first.readUInt16LE(8);
  const out = Buffer.alloc(total);
  let got = first.readUInt16LE(6);
  first.copy(out, 0, 14, 14 + got);
  while (got < total) {
    const b = mkReq(got, chunkMax);
    const n = b.readUInt16LE(6);
    if (n === 0) break;
    b.copy(out, got, 14, 14 + n);
    got += n;
  }
  return out;
}

function readSlot(link, slot, maxBody) {
  return readStream(link, (off, max) => {
    const req = Buffer.alloc(7);
    req[0] = slot;
    req.writeUInt32LE(off, 1);
    req.writeUInt16LE(max, 5);
    return link.request(CMD.readSlot, req);
  }, maxBody);
}

function getLive(link, maxBody) {
  return readStream(link, (off, max) => {
    const req = Buffer.alloc(6);
    req.writeUInt32LE(off, 0);
    req.writeUInt16LE(max, 4);
    return link.request(CMD.getLive, req);
  }, maxBody);
}

function writeBlob(link, target, bytes, schemaHash, maxBody) {
  const begin = Buffer.alloc(9);
  begin[0] = target;
  begin.writeUInt32LE(bytes.length, 1);
  begin.writeUInt32LE(schemaHash >>> 0, 5);
  link.request(CMD.blobBegin, begin);

  const chunkMax = maxBody - 4;
  for (let off = 0; off < bytes.length; off += chunkMax) {
    const n = Math.min(chunkMax, bytes.length - off);
    const data = Buffer.alloc(4 + n);
    data.writeUInt32LE(off, 0);
    bytes.copy(data, 4, off, off + n);
    link.request(CMD.blobData, data);
  }
  const commit = Buffer.alloc(4);
  commit.writeUInt32LE(crc32(bytes), 0);
  link.request(CMD.blobCommit, commit, 12000);
}

/* ── CLI ────────────────────────────────────────────────────────────── */

function parseArgs(argv) {
  const args = { _: [] };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === "-p" || a === "--port") args.port = argv[++i];
    else args._.push(a);
  }
  return args;
}

function usage() {
  console.error(`hostlink.mjs — HostLink bench CLI

  hostlink.mjs [-p PORT] <command> [args]

Commands:
  hello                     print device identity
  descriptor                print descriptor JSON (stdout)
  list                      list slot table
  read <slot>               read slot blob to stdout (binary)
  write <slot> <file>       write a blob file to a slot
  getlive                   read live state to stdout (binary)
  setlive <file>            audition a blob (volatile)
  save <slot>               SAVE_TO_SLOT (on-device gesture)
  load <slot>               LOAD_FROM_SLOT (on-device gesture)
  erase <slot>              invalidate a slot
  reboot [app|bootloader]   reset the module
`);
}

/** Validate this file's codecs against the SDK golden vectors (no
 *  hardware).  Proves the bench tool is byte-compatible with firmware. */
function selftest() {
  const golden = JSON.parse(
    readFileSync(new URL("../../tests/host/golden/hostlink_golden.json", import.meta.url)),
  );
  let fail = 0;
  const hex = (b) => Buffer.from(b).toString("hex");
  for (const v of golden.crc32) {
    if (crc32(Buffer.from(v.hex, "hex")) !== v.value) { console.error(`crc32 FAIL ${v.hex}`); fail++; }
  }
  for (const v of golden.cobs) {
    if (hex(cobsEncode(Buffer.from(v.dec, "hex"))) !== v.enc) { console.error(`cobs FAIL ${v.dec}`); fail++; }
    const dec = cobsDecode(Buffer.from(v.enc, "hex"));
    if (!dec || hex(dec) !== v.dec) { console.error(`cobs-decode FAIL ${v.enc}`); fail++; }
  }
  // Rebuild the first recorded host request (HELLO, seq 1) from scratch.
  const h2d0 = golden.exchange[0];
  if (h2d0.dir === "h2d" && hex(buildFrame(CMD.hello, 1, Buffer.alloc(0))) !== h2d0.hex) {
    console.error("frame FAIL: HELLO seq1 mismatch"); fail++;
  }
  // Parse the first recorded device response and check status ok.
  const p = new FrameParser();
  let parsed = null;
  for (const byte of Buffer.from(golden.exchange[1].hex, "hex")) for (const f of p.push(byte)) parsed = f;
  if (!parsed || !parsed.ok || parsed.body[0] !== 0) { console.error("frame FAIL: HELLO response parse"); fail++; }
  console.log(fail === 0 ? "selftest: OK (codecs match golden vectors)" : `selftest: ${fail} FAILURES`);
  process.exit(fail === 0 ? 0 : 1);
}

function main() {
  const args = parseArgs(process.argv.slice(2));
  const cmd = args._[0];
  if (cmd === "selftest") return selftest();
  if (!cmd || cmd === "help" || cmd === "-h" || cmd === "--help") { usage(); process.exit(cmd ? 0 : 1); }

  const port = args.port ?? defaultPort();
  const link = new SerialLink(port);
  try {
    const info = hello(link); // always handshake first — establishes maxBody
    switch (cmd) {
      case "hello":
        console.log(JSON.stringify(info, null, 2));
        break;
      case "descriptor":
        process.stdout.write(getDescriptor(link, info));
        break;
      case "list": {
        const slots = listSlots(link);
        for (const s of slots) {
          const tag = !s.valid ? "empty" : s.schemaMatch ? "ok" : "FOREIGN-SCHEMA";
          console.log(
            `slot ${String(s.slot + 1).padStart(2)}  ${tag.padEnd(14)}` +
              (s.valid ? `  seq=${s.seq} len=${s.length} schema=0x${s.schemaHash.toString(16)}` : ""),
          );
        }
        break;
      }
      case "read": {
        const slot = Number(args._[1]) - 1;
        process.stdout.write(readSlot(link, slot, info.maxBody));
        break;
      }
      case "write": {
        const slot = Number(args._[1]) - 1;
        const bytes = readFileSync(args._[2]);
        writeBlob(link, slot, bytes, info.schemaHash, info.maxBody);
        console.error(`wrote ${bytes.length} bytes to slot ${slot + 1}`);
        break;
      }
      case "getlive":
        process.stdout.write(getLive(link, info.maxBody));
        break;
      case "setlive": {
        const bytes = readFileSync(args._[1]);
        writeBlob(link, TARGET_LIVE, bytes, info.schemaHash, info.maxBody);
        console.error(`auditioned ${bytes.length} bytes (volatile)`);
        break;
      }
      case "save":
        link.request(CMD.saveToSlot, Buffer.from([Number(args._[1]) - 1]), 12000);
        console.error(`saved live → slot ${args._[1]}`);
        break;
      case "load":
        link.request(CMD.loadFromSlot, Buffer.from([Number(args._[1]) - 1]));
        console.error(`loaded slot ${args._[1]} → live`);
        break;
      case "erase":
        link.request(CMD.eraseSlot, Buffer.from([Number(args._[1]) - 1]), 12000);
        console.error(`erased slot ${args._[1]}`);
        break;
      case "reboot": {
        const mode = args._[1] === "bootloader" ? 1 : 0;
        link.request(CMD.reboot, Buffer.from([mode]));
        console.error(`reboot (${mode ? "bootloader" : "app"}) acknowledged`);
        break;
      }
      default:
        usage();
        process.exitCode = 1;
    }
  } finally {
    link.close();
  }
}

main();
