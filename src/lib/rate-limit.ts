import { db } from "@/lib/db";

/**
 * Fixed-window counters kept in Postgres.
 *
 * A `Map` in module scope only bounds the serverless instance that happens to serve a request, so
 * it cannot limit anything: every new instance starts with an empty map. These rows are shared by
 * all instances instead.
 */

/** Swept opportunistically, so abandoned keys do not accumulate. */
const SWEEP_AFTER_MS = 24 * 60 * 60 * 1000;
let lastSweep = 0;

/**
 * Counts one event against `key` and reports whether it stayed within `limit` per `windowMs`.
 * Returns true when the caller may proceed.
 */
export async function withinLimit(key: string, limit: number, windowMs: number): Promise<boolean> {
  const now = new Date();
  const windowStart = new Date(now.getTime() - windowMs);
  try {
    // Reset the window first: an expired row starts counting again from this request.
    await db.rateLimit.updateMany({
      where: { key, windowStart: { lt: windowStart } },
      data: { count: 0, windowStart: now },
    });
    const row = await db.rateLimit.upsert({
      where: { key },
      create: { key, count: 1, windowStart: now },
      update: { count: { increment: 1 } },
      select: { count: true, windowStart: true },
    });
    void sweep(now);
    // A row that rolled over between the two statements is counted against the fresh window.
    return row.windowStart < windowStart ? true : row.count <= limit;
  } catch (error) {
    // A counter must never take the feature down; log and let the request through.
    console.error("Rate limit unavailable:", error);
    return true;
  }
}

/** Forgets a key, e.g. after the attempt it was counting finally succeeded. */
export async function clearLimit(key: string) {
  await db.rateLimit.deleteMany({ where: { key } }).catch(() => {});
}

async function sweep(now: Date) {
  if (now.getTime() - lastSweep < 60 * 60 * 1000) return;
  lastSweep = now.getTime();
  await db.rateLimit
    .deleteMany({ where: { windowStart: { lt: new Date(now.getTime() - SWEEP_AFTER_MS) } } })
    .catch(() => {});
}
