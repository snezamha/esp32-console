import { createHash } from "node:crypto";

/** A stable, non-plain-text owner key for a verified Google email address. */
export function ownerIdForEmail(email: string) {
  const normalized = email.trim().toLowerCase();
  return normalized ? `google-email:${createHash("sha256").update(normalized).digest("hex")}` : "";
}
