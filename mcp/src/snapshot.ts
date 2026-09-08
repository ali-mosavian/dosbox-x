export const MEM_READ_CHUNK = 4096;
export const MAX_DIFF_RANGES = 64;
export const MAX_HEX_BYTES_PER_RANGE = 256;

export interface DiffRange {
  start: number;
  end: number;
  oldHex: string;
  newHex: string;
}

export interface DiffResult {
  ranges: DiffRange[];
  truncated: boolean;
  totalDirtyBytes: number;
  dirtyRangeCount: number;
}

function bytesToHex(bytes: Uint8Array, maxBytes: number): string {
  const limit = Math.min(bytes.length, maxBytes);
  let hex = "";
  for (let index = 0; index < limit; index++) {
    hex += bytes[index].toString(16).padStart(2, "0").toUpperCase();
  }
  return hex;
}

export function coalesceMemoryDiff(
  oldBytes: Uint8Array,
  newBytes: Uint8Array,
  baseAddr: number,
  maxRanges = MAX_DIFF_RANGES,
  maxHexBytes = MAX_HEX_BYTES_PER_RANGE,
): DiffResult {
  const length = Math.min(oldBytes.length, newBytes.length);
  const ranges: DiffRange[] = [];
  let totalDirtyBytes = 0;
  let index = 0;

  while (index < length) {
    while (index < length && oldBytes[index] === newBytes[index]) index++;
    if (index >= length) break;

    const start = index;
    while (index < length && oldBytes[index] !== newBytes[index]) index++;
    const end = index;
    totalDirtyBytes += end - start;

    if (ranges.length < maxRanges) {
      const oldSlice = oldBytes.subarray(start, end);
      const newSlice = newBytes.subarray(start, end);
      ranges.push({
        start: (baseAddr + start) >>> 0,
        end: (baseAddr + end) >>> 0,
        oldHex: bytesToHex(oldSlice, maxHexBytes),
        newHex: bytesToHex(newSlice, maxHexBytes),
      });
    }
  }

  const dirtyRangeCount = countDirtyRanges(oldBytes, newBytes);
  return {
    ranges,
    truncated: dirtyRangeCount > maxRanges,
    totalDirtyBytes,
    dirtyRangeCount,
  };
}

function countDirtyRanges(oldBytes: Uint8Array, newBytes: Uint8Array): number {
  const length = Math.min(oldBytes.length, newBytes.length);
  let count = 0;
  let index = 0;
  while (index < length) {
    while (index < length && oldBytes[index] === newBytes[index]) index++;
    if (index >= length) break;
    count++;
    while (index < length && oldBytes[index] !== newBytes[index]) index++;
  }
  return count;
}

export function hexDataToBytes(data: string): Uint8Array {
  const tokens = data.match(/.{1,2}/g) ?? [];
  const bytes = new Uint8Array(tokens.length);
  for (let index = 0; index < tokens.length; index++) {
    const token = tokens[index];
    if (token === "PF" || token === "??") {
      throw new Error(`Unreadable byte token ${token} in memory read`);
    }
    bytes[index] = Number.parseInt(token, 16);
  }
  return bytes;
}

export async function readLinearMemoryChunked(
  readChunk: (addr: number, len: number) => Promise<Uint8Array>,
  addr: number,
  len: number,
  chunkSize = MEM_READ_CHUNK,
): Promise<Uint8Array> {
  const out = new Uint8Array(len);
  let offset = 0;
  while (offset < len) {
    const piece = Math.min(chunkSize, len - offset);
    const chunk = await readChunk((addr + offset) >>> 0, piece);
    out.set(chunk.subarray(0, Math.min(chunk.length, piece)), offset);
    offset += piece;
  }
  return out;
}
