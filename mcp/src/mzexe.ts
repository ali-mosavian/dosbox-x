import { readFileSync } from "fs";

export interface MzHeader {
  extraBytes: number;
  pages: number;
  relocationCount: number;
  headerParagraphs: number;
  minAlloc: number;
  maxAlloc: number;
  initSS: number;
  initSP: number;
  checksum: number;
  initIP: number;
  initCS: number;
  relocationTableOffset: number;
  overlay: number;
}

export interface MzImage {
  header: MzHeader;
  /** File offset of the load image (past the MZ header). */
  imageOffset: number;
  /** Load image length in bytes, as the DOS loader computes it. */
  imageSize: number;
  /** File offset one past the load image: where appended debug info starts. */
  appendedOffset: number;
  fileSize: number;
}

export function parseMzHeader(data: Buffer): MzHeader | undefined {
  if (data.length < 28) return undefined;
  const signature = data.readUInt16LE(0);
  // ZM is the rarer byte-swapped spelling some early linkers emitted.
  if (signature !== 0x5a4d && signature !== 0x4d5a) return undefined;
  return {
    extraBytes: data.readUInt16LE(2),
    pages: data.readUInt16LE(4),
    relocationCount: data.readUInt16LE(6),
    headerParagraphs: data.readUInt16LE(8),
    minAlloc: data.readUInt16LE(10),
    maxAlloc: data.readUInt16LE(12),
    initSS: data.readUInt16LE(14),
    initSP: data.readUInt16LE(16),
    checksum: data.readUInt16LE(18),
    initIP: data.readUInt16LE(20),
    initCS: data.readUInt16LE(22),
    relocationTableOffset: data.readUInt16LE(24),
    overlay: data.readUInt16LE(26),
  };
}

export function mzImage(data: Buffer): MzImage | undefined {
  const header = parseMzHeader(data);
  if (header === undefined) return undefined;

  const imageOffset = header.headerParagraphs << 4;
  // e_cblp is how much of the LAST page is used; zero means the page is full.
  const lastPage = header.extraBytes === 0 ? 512 : header.extraBytes;
  const pagedSize = header.pages === 0 ? 0 : ((header.pages - 1) * 512 + lastPage);
  const imageSize = Math.max(0, pagedSize - imageOffset);
  const appendedOffset = Math.min(imageOffset + imageSize, data.length);

  return {
    header,
    imageOffset,
    imageSize,
    appendedOffset,
    fileSize: data.length,
  };
}

export function readMzImage(file: string): MzImage | undefined {
  return mzImage(readFileSync(file));
}

/**
 * The header fields DOS EXEC reports back through loadInfo, joined.
 *
 * Matching a host file to the program the guest launched by name alone is
 * wrong the moment two build directories hold the same basename, which is
 * this project's normal working state.
 */
export function mzFingerprint(header: MzHeader): string {
  return [
    header.extraBytes,
    header.pages,
    header.relocationCount,
    header.headerParagraphs,
    header.minAlloc,
    header.maxAlloc,
    header.initSS,
    header.initSP,
    header.initIP,
    header.initCS,
    header.relocationTableOffset,
  ].join(":");
}
