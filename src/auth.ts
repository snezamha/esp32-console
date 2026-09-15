import NextAuth from "next-auth";
import Google from "next-auth/providers/google";
import { ownerIdForEmail } from "@/lib/owner-id";

/**
 * Google sign-in only, JWT session (no database adapter — the device owner is the Google
 * account's verified email). Set GOOGLE_CLIENT_ID, GOOGLE_CLIENT_SECRET and AUTH_SECRET
 * in the environment (see README).
 */
export const { handlers, auth, signIn, signOut } = NextAuth({
  providers: [Google],
  session: { strategy: "jwt" },
  // Vercel sets this itself; needed for other hosts/proxies to trust the request's own Host header.
  trustHost: true,
  callbacks: {
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
