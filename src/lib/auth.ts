import { auth } from "@/auth";
import { emailAllowed } from "@/lib/allowlist";
import { db } from "@/lib/db";

/** Owner keys whose legacy rows were already moved. Bounded so a long-lived instance cannot grow
 * this without limit; a dropped entry only costs one extra no-op update. */
const MIGRATED_CACHE_MAX = 500;
const migratedSessions = new Set<string>();

/** The signed-in user (Google account), or null. `id` is the stable owner key for devices. */
export async function getUser() {
  const session = await auth();
  if (!session?.user?.id) return null;
  // Re-checked on every request, so sessions issued before AUTH_ALLOWED_EMAILS was set (or before
  // an address was removed from it) stop working without waiting for the JWT to expire.
  if (!emailAllowed(session.user.email ?? "")) return null;

  const legacyOwnerId = session.user.legacyOwnerId;
  const migrationKey = legacyOwnerId ? `${legacyOwnerId}:${session.user.id}` : "";
  if (migrationKey && !migratedSessions.has(migrationKey)) {
    await db.device.updateMany({
      where: { owner: legacyOwnerId },
      data: { owner: session.user.id },
    });
    if (migratedSessions.size >= MIGRATED_CACHE_MAX) migratedSessions.clear();
    migratedSessions.add(migrationKey);
  }

  return { id: session.user.id, name: session.user.name ?? "", email: session.user.email ?? "" };
}

export const unauthorized = () => Response.json({ error: "Sign in first." }, { status: 401 });
