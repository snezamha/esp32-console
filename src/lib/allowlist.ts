/**
 * Who may sign in. `AUTH_ALLOWED_EMAILS` is a comma-separated list of full addresses
 * (`me@example.com`) and/or whole domains (`@example.com`), matched case-insensitively.
 *
 * Leaving it unset keeps the console open to every Google account, which also opens Project
 * Builder's server-side compiler to them — set it on any deployment reachable from the internet.
 */
const entries = () =>
  (process.env.AUTH_ALLOWED_EMAILS ?? "")
    .split(",")
    .map((entry) => entry.trim().toLowerCase())
    .filter(Boolean);

export const allowlistConfigured = () => entries().length > 0;

export function emailAllowed(email: string) {
  const normalized = email.trim().toLowerCase();
  if (!normalized || !normalized.includes("@")) return false;
  const allowed = entries();
  if (!allowed.length) return true;
  const domain = normalized.slice(normalized.lastIndexOf("@"));
  return allowed.some((entry) => (entry.startsWith("@") ? entry === domain : entry === normalized));
}
