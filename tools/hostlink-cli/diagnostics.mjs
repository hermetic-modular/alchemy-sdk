import { setTimeout as delay } from "node:timers/promises";
import {
  decodeDiagnosticsInfo, decodeLogBatch, decodeGaugePage, decodeGaugeSnapshot,
  diagnosticsNonce, formatGaugeValue, LOG_LEVELS,
} from "./diagnostics-codec.mjs";

function sessionRequest(session, length = 4) {
  const req = Buffer.alloc(length);
  req.writeUInt32LE(session, 0);
  return req;
}

/** Safe read retry: diagnostics reads never consume or clear records. */
function request(link, cmd, body) {
  try { return link.request(cmd, body); }
  catch (err) {
    if (err.code === "timeout" || err.status === 10) return link.request(cmd, body);
    if (err.status === 1) throw new Error("This firmware does not support diagnostics.");
    throw err;
  }
}

function info(link) {
  return decodeDiagnosticsInfo(request(link, 0x60, sessionRequest(diagnosticsNonce())));
}

function sameSession(expected, received) {
  if (expected !== received) throw Object.assign(new Error("Device restarted"), { status: 3 });
}

function describe(link, device) {
  const definitions = [];
  while (definitions.length < device.gaugeCount) {
    const req = sessionRequest(device.session, 5);
    req[4] = definitions.length;
    const page = decodeGaugePage(request(link, 0x62, req));
    sameSession(device.session, page.session);
    if (!page.definitions.length || page.next !== definitions.length + page.definitions.length
        || page.next > device.gaugeCount) throw new Error("Invalid diagnostics descriptor page");
    definitions.push(...page.definitions);
  }
  return definitions;
}

// Device text is data, including when it contains terminal escape sequences.
export function terminalText(text) {
  return text.replace(/[\x00-\x1f\x7f-\x9f]/g, (c) => `\\x${c.charCodeAt(0).toString(16).padStart(2, "0")}`);
}

export async function runDiagnostics(link, command, args) {
  const minimum = LOG_LEVELS.indexOf(args.level ?? "debug");
  if (minimum < 0) throw new Error("--level must be debug, info, warn, or error");
  let device = info(link);
  let cursor = device.oldest;
  let definitions = command === "watch" ? describe(link, device) : [];
  let previousRows = 0;
  const stop = new AbortController();
  const onStop = () => stop.abort();
  process.on("SIGINT", onStop);
  process.on("SIGTERM", onStop);
  const emit = (event, text) => {
    if (args.json) console.log(JSON.stringify(event));
    else console.log(terminalText(text));
  };
  if (device.configurationError) console.error("The firmware reports a diagnostics registration error.");
  if (device.dropped) emit({ type: "gap", session: device.session, lost: device.dropped },
                          `[${device.dropped} earlier messages are no longer retained]`);
  try {
    do {
      try {
        if (command === "logs") {
          const req = sessionRequest(device.session, 10);
          req.writeUInt32LE(cursor, 4); req.writeUInt16LE(32, 8);
          const batch = decodeLogBatch(request(link, 0x61, req));
          sameSession(device.session, batch.session);
          if (batch.next !== cursor + batch.lost + batch.records.length)
            throw new Error("Invalid diagnostics cursor progression");
          cursor = batch.next;
          if (batch.lost) emit({ type: "gap", session: device.session, lost: batch.lost },
                               `[${batch.lost} messages lost]`);
          for (const record of batch.records) {
            if (record.level < minimum || (!args.follow && record.sequence >= device.next)) continue;
            emit({ type: "log", session: device.session, ...record },
                 `${(record.timeMs / 1000).toFixed(3).padStart(10)}  ${(LOG_LEVELS[record.level] ?? "unknown").toUpperCase().padEnd(7)} ${record.text}${record.truncated ? " [truncated]" : ""}`);
          }
          // A one-shot read drains exactly the history observed at discovery,
          // even if the firmware keeps producing records while we read it.
          if (!args.follow && cursor >= device.next) break;
        } else {
          const snapshot = decodeGaugeSnapshot(request(link, 0x63, sessionRequest(device.session)));
          sameSession(device.session, snapshot.session);
          const values = definitions.map((d) => ({ ...d, value: snapshot.values[d.index]?.value ?? null }));
          if (args.json) console.log(JSON.stringify({ type: "values", ...snapshot, values }));
          else {
            const rows = values.map((g) => `${terminalText(g.label).padEnd(28)} ${formatGaugeValue(g.value).padStart(12)} ${terminalText(g.unit)}`);
            rows.unshift(`Device values at ${(snapshot.timeMs / 1000).toFixed(3)} s`);
            if (!values.length) rows.push("No live values registered by this firmware.");
            if (process.stdout.isTTY && previousRows) process.stdout.write(`\x1b[${previousRows}A\x1b[J`);
            process.stdout.write(rows.join("\n") + "\n");
            previousRows = rows.length;
          }
        }
      } catch (err) {
        if (err.status !== 3) throw err;
        device = info(link); cursor = device.oldest;
        definitions = command === "watch" ? describe(link, device) : [];
        emit({ type: "restart", session: device.session }, "[Device restarted]");
      }
      if (stop.signal.aborted) break;
      if (args.follow || command === "watch")
        await delay(200, undefined, { signal: stop.signal }).catch((err) => {
          if (err.name !== "AbortError") throw err;
        });
    } while (!stop.signal.aborted);
  } finally {
    process.off("SIGINT", onStop);
    process.off("SIGTERM", onStop);
  }
}
