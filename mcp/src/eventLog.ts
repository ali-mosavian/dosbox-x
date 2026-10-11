import type { JsonObject } from "./dosbox.js";

/** Events the emulator sends on its own (key_wait, image_loaded, profile ...), kept so a tool can wait for the next one. */
export class EventLog {
  private entries: { seq: number; event: JsonObject }[] = [];
  private waiters: { name: string | undefined; after: number; resolve: (e: JsonObject) => void; timer: NodeJS.Timeout }[] = [];
  private seq = 0;

  constructor(private readonly keep = 200) {}

  /** The number the next event will have; pass it to wait() to wait for one after now. */
  mark(): number {
    return this.seq;
  }

  push(event: JsonObject): void {
    const entry = { seq: ++this.seq, event };
    this.entries.push(entry);
    if (this.entries.length > this.keep) this.entries.shift();
    for (const waiter of [...this.waiters]) {
      if (waiter.name !== undefined && waiter.name !== event["event"]) continue;
      if (entry.seq <= waiter.after) continue;
      clearTimeout(waiter.timer);
      this.waiters = this.waiters.filter((w) => w !== waiter);
      waiter.resolve(event);
    }
  }

  list(name?: string): JsonObject[] {
    return this.entries.filter((e) => name === undefined || e.event["event"] === name).map((e) => e.event);
  }

  clear(): void {
    this.entries = [];
  }

  /** The first event (of `name`) with a number above `after`, already seen or still to come; undefined at the timeout. */
  wait(name: string | undefined, after: number, timeoutMs: number): Promise<JsonObject | undefined> {
    const seen = this.entries.find((e) => e.seq > after && (name === undefined || e.event["event"] === name));
    if (seen !== undefined) return Promise.resolve(seen.event);
    return new Promise((resolve) => {
      const waiter = {
        name,
        after,
        resolve: (e: JsonObject) => resolve(e),
        timer: setTimeout(() => {
          this.waiters = this.waiters.filter((w) => w !== waiter);
          resolve(undefined);
        }, timeoutMs),
      };
      this.waiters.push(waiter);
    });
  }
}
