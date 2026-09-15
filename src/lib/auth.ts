import { auth } from "@/auth";
import { db } from "@/lib/db";

const migratedSessions = new Set<string>();

/** The signed-in user (Google account), or null. `id` is the stable owner key for devices. */
export async function getUser() {
  const session = await auth();
  if (!session?.user?.id) return null;

  const legacyOwnerId = session.user.legacyOwnerId;
  const migrationKey = legacyOwnerId ? `${legacyOwnerId}:${session.user.id}` : "";
  if (migrationKey && !migratedSessions.has(migrationKey)) {
    await db.device.updateMany({
      where: { owner: legacyOwnerId },
      data: { owner: session.user.id },
    });
    migratedSessions.add(migrationKey);
  }

  return { id: session.user.id, name: session.user.name ?? "", email: session.user.email ?? "" };
}

export const unauthorized = () => Response.json({ error: "Sign in first." }, { status: 401 });
