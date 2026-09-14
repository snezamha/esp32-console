import { auth } from "@/auth";

/** The signed-in user (Google account), or null. `id` is the stable owner key for devices. */
export async function getUser() {
  const session = await auth();
  return session?.user?.id ? { id: session.user.id, name: session.user.name ?? "", email: session.user.email ?? "" } : null;
}

export const unauthorized = () => Response.json({ error: "Sign in first." }, { status: 401 });
