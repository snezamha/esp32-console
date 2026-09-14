import NextAuth from "next-auth";
import Google from "next-auth/providers/google";

/**
 * Google sign-in only, JWT session (no database adapter — the device owner is the Google
 * account's stable id, `token.sub`). Set GOOGLE_CLIENT_ID, GOOGLE_CLIENT_SECRET and AUTH_SECRET
 * in the environment (see README).
 */
export const { handlers, auth, signIn, signOut } = NextAuth({
  providers: [Google],
  session: { strategy: "jwt" },
  // Vercel sets this itself; needed for other hosts/proxies to trust the request's own Host header.
  trustHost: true,
  callbacks: {
    // Exposes the stable Google account id to the app as session.user.id (the device owner key).
    session({ session, token }) {
      if (token.sub) session.user.id = token.sub;
      return session;
    },
  },
});
