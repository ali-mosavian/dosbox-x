/** Turning tool results into the text the MCP hands back. */

/**
 * Pretty JSON for a tool reply.
 *
 * A Promise is written out as a marker rather than as JSON.stringify's `{}`:
 * dosbox_where reported `"sourceLine": {}` for months of stops because the
 * source-line lookup became async and one caller stopped awaiting it, and an
 * empty object is indistinguishable from "this address has no line".
 */
export function j(val: unknown): string {
  return JSON.stringify(
    val,
    (_key, value) => (value instanceof Promise ? "<unawaited Promise>" : value),
    2,
  );
}
