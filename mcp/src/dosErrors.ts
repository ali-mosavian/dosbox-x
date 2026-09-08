const DOS_ERROR_PATTERNS: RegExp[] = [
  /bad command or file name/i,
  /file not found/i,
  /invalid directory/i,
  /illegal command/i,
  /abort, retry, ignore, fail/i,
  /cannot find/i,
  /\bsevere\b/i,
  /\berror\b/i,
];

export function scrapeDosErrors(text: string): string[] {
  const hits = new Set<string>();
  for (const line of text.split(/\r?\n/)) {
    const trimmed = line.trim();
    if (trimmed.length === 0) continue;
    for (const pattern of DOS_ERROR_PATTERNS) {
      if (pattern.test(trimmed)) {
        hits.add(trimmed);
        break;
      }
    }
  }
  return [...hits];
}
