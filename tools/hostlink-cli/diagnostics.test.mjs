import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import {
  decodeDiagnosticsInfo, decodeLogBatch, decodeGaugePage, decodeGaugeSnapshot,
} from "./diagnostics-codec.mjs";
import { runDiagnostics, terminalText } from "./diagnostics.mjs";

const golden = JSON.parse(readFileSync(new URL("../../tests/host/golden/diagnostics_golden.json", import.meta.url)));
function body(hex) {
  const wire = Buffer.from(hex, "hex");
  const decoded = [];
  for (let i = 0; i < wire.length - 1;) {
    const code = wire[i++];
    for (let n = 1; n < code; ++n) decoded.push(wire[i++]);
    if (code !== 255 && i < wire.length - 1) decoded.push(0);
  }
  return Buffer.from(decoded).subarray(6, -4);
}
const [info, logs, definitions, values] = golden.exchange.map((e) => body(e.response));

test("matches real firmware logs and each gauge type", () => {
  assert.equal(decodeDiagnosticsInfo(info).session, 0x12345678);
  assert.deepEqual(decodeLogBatch(logs).records.map((r) => r.text), ["Ready", "Preset 3 used defaults"]);
  assert.equal(decodeGaugePage(definitions).definitions[0].unit, "%");
  assert.deepEqual(decodeGaugeSnapshot(values).values.map((v) => v.value), [23.5, -7, 4294967295, true]);
});

test("rejects all truncated golden response prefixes", () => {
  const decoders = [decodeDiagnosticsInfo, decodeLogBatch, decodeGaugePage, decodeGaugeSnapshot];
  [info, logs, definitions, values].forEach((b, i) => {
    for (let n = 0; n < b.length; ++n) assert.throws(() => decoders[i](b.subarray(0, n)));
  });
});

test("CLI filters records but drains its cursor and emits valid JSON Lines", async (t) => {
  const output = [];
  t.mock.method(console, "log", (line) => output.push(line));
  const calls = [];
  await runDiagnostics({ request: (cmd, req) => {
    calls.push(cmd);
    if (cmd === 0x60) { assert.notEqual(req.readUInt32LE(), 0); return info; }
    assert.equal(req.readUInt32LE(), 0x12345678);
    assert.equal(req.readUInt32LE(4), 1);
    return logs;
  } }, "logs", { json: true, level: "warn" });
  assert.deepEqual(calls, [0x60, 0x61]);
  assert.equal(output.length, 1);
  assert.deepEqual(JSON.parse(output[0]), {
    type: "log", session: 0x12345678, sequence: 2, timeMs: 1234,
    level: 2, text: "Preset 3 used defaults", truncated: false,
  });
});

test("CLI retries a lost diagnostics response", async (t) => {
  t.mock.method(console, "log", () => {});
  let reads = 0;
  await runDiagnostics({ request: (cmd) => {
    if (cmd === 0x60) return info;
    if (++reads === 1) throw Object.assign(new Error("lost reply"), { code: "timeout" });
    return logs;
  } }, "logs", {});
  assert.equal(reads, 2);
});

test("unsupported firmware and invalid filtering fail clearly", async () => {
  await assert.rejects(runDiagnostics({ request: () => { throw Object.assign(new Error(), { status: 1 }); } }, "logs", {}), /does not support/);
  await assert.rejects(runDiagnostics({}, "logs", { level: "verbose" }), /--level/);
});

test("device text cannot inject terminal escape sequences", () => {
  assert.equal(terminalText("ready\x1b[2J\nnext"), "ready\\x1b[2J\\x0anext");
});
