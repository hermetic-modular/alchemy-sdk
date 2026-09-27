export const LOG_LEVELS = ["debug", "info", "warn", "error"];
class Reader {
    bytes;
    at = 0;
    constructor(bytes) {
        this.bytes = bytes;
    }
    take(n) {
        if (this.at + n > this.bytes.length)
            throw new Error("Truncated diagnostics response");
        const result = this.bytes.subarray(this.at, this.at + n);
        this.at += n;
        return result;
    }
    u8() { return this.take(1)[0]; }
    u16() { const b = this.take(2); return b[0] | (b[1] << 8); }
    u32() {
        const b = this.take(4);
        return new DataView(b.buffer, b.byteOffset, 4).getUint32(0, true);
    }
    text(n) { return new TextDecoder().decode(this.take(n)); }
    str() { return this.text(this.u8()); }
    ok() { if (this.u8() !== 0)
        throw new Error("Diagnostics request failed"); }
}
export function decodeDiagnosticsInfo(body) {
    const r = new Reader(body);
    r.ok();
    const version = r.u8(), flags = r.u8();
    if (version !== 1)
        throw new Error(`Unsupported diagnostics version ${version}`);
    const result = {
        version, logs: !!(flags & 1), values: !!(flags & 2), configurationError: !!(flags & 4),
        session: r.u32(), oldest: r.u32(), next: r.u32(), dropped: r.u32(),
        truncated: r.u32(), formatErrors: r.u32(), capacity: r.u16(), maxText: r.u16(),
        gaugeCount: r.u8(),
    };
    if (!result.session || !result.oldest || result.next < result.oldest
        || !result.capacity || result.next - result.oldest > result.capacity)
        throw new Error("Invalid diagnostics history bounds");
    return result;
}
export function decodeLogBatch(body) {
    const r = new Reader(body);
    r.ok();
    const session = r.u32(), next = r.u32(), lost = r.u32(), count = r.u16();
    const records = [];
    for (let i = 0; i < count; ++i) {
        const sequence = r.u32(), timeMs = r.u32(), level = r.u8(), flags = r.u8();
        const text = r.text(r.u16());
        if (!sequence || sequence !== next - count + i)
            throw new Error("Invalid diagnostics log cursor");
        records.push({ sequence, timeMs, level, truncated: !!(flags & 1), text });
    }
    if (!session || !next)
        throw new Error("Invalid diagnostics session or cursor");
    return { session, next, lost, records };
}
export function decodeGaugePage(body) {
    const r = new Reader(body);
    r.ok();
    const session = r.u32(), next = r.u8(), count = r.u8();
    const definitions = [];
    for (let i = 0; i < count; ++i) {
        const index = r.u8(), type = r.u8(), id = r.str(), label = r.str(), unit = r.str();
        if (index !== next - count + i || !id)
            throw new Error("Invalid diagnostics value definition");
        definitions.push({ index, type, id, label, unit });
    }
    return { session, next, definitions };
}
export function decodeGaugeSnapshot(body) {
    const r = new Reader(body);
    r.ok();
    const session = r.u32(), timeMs = r.u32(), count = r.u8();
    const values = [];
    for (let i = 0; i < count; ++i) {
        const index = r.u8(), type = r.u8(), flags = r.u8(), bits = r.take(4);
        const view = new DataView(bits.buffer, bits.byteOffset, 4);
        let value = null;
        if (flags & 1) {
            if (type === 1) {
                const n = view.getFloat32(0, true);
                value = Number.isFinite(n) ? n : null;
            }
            else if (type === 2)
                value = view.getInt32(0, true);
            else if (type === 3)
                value = view.getUint32(0, true);
            else if (type === 4)
                value = view.getUint32(0, true) !== 0;
        }
        if (index !== i)
            throw new Error("Invalid diagnostics value index");
        values.push({ index, type, value });
    }
    return { session, timeMs, values };
}
/** A fresh candidate on each discovery; the device keeps its first token until reboot. */
export function diagnosticsNonce() {
    const nonce = new Uint32Array(1);
    crypto.getRandomValues(nonce);
    return nonce[0] || 1;
}
export function formatGaugeValue(value) {
    if (value === null)
        return "No value";
    if (typeof value === "boolean")
        return value ? "Yes" : "No";
    return Number.isInteger(value) ? String(value) : String(Number(value.toPrecision(6)));
}
