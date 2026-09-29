import NextAuth from "next-auth";
import Google from "next-auth/providers/google";
import { allowlistConfigured, emailAllowed } from "@/lib/allowlist";
import { ownerIdForEmail } from "@/lib/owner-id";

let warned = false;

/**
 * Google sign-in only, JWT session (no database adapter — the device owner is the Google
 * account's verified email). Set GOOGLE_CLIENT_ID, GOOGLE_CLIENT_SECRET and AUTH_SECRET
 * in the environment (see README).
 *
 * `AUTH_ALLOWED_EMAILS` restricts who may sign in at all. Without it every Google account is
 * accepted, so a public deployment should always set it.
 */
export const { handlers, auth, signIn, signOut } = NextAuth({
  providers: [Google],
  session: { strategy: "jwt" },
  // Vercel sets this itself; needed for other hosts/proxies to trust the request's own Host header.
  trustHost: true,
  callbacks: {
    // Ownership is derived from the email, so an unverified one must never reach a session: it
    // would let a self-hosted Google account claim another person's owner key.
    signIn({ profile, user }) {
      const email = profile?.email ?? user?.email ?? "";
      if (profile && profile.email_verified === false) return false;
      if (!allowlistConfigured() && !warned) {
        warned = true;
        console.warn("AUTH_ALLOWED_EMAILS is not set: every Google account can sign in to this console.");
      }
      return emailAllowed(email);
    },
    // Auth.js generates token.sub without an adapter, so it can change after another sign-in.
    // Keep it only as a legacy key while all new ownership uses the verified Google email hash.
    session({ session, token }) {
      const ownerId = ownerIdForEmail(session.user.email ?? "");
      if (ownerId) session.user.id = ownerId;
      if (token.sub && token.sub !== ownerId) session.user.legacyOwnerId = token.sub;
      return session;
    },
  },
});
