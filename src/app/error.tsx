"use client";

import { useEffect } from "react";

/** Replaces the raw Next.js failure screen when a page or a server component throws. */
export default function Error({ error, reset }: { error: Error & { digest?: string }; reset: () => void }) {
  useEffect(() => {
    console.error("Console page failed:", error);
  }, [error]);

  return (
    <main className="mx-auto flex min-h-dvh w-full max-w-md flex-col items-center justify-center gap-4 px-6 text-center">
      <h1 className="text-base font-semibold">Something went wrong</h1>
      <p className="text-sm text-zinc-500">
        The console could not load this view. Check that the database is reachable, then try again.
      </p>
      {error.digest && <p className="font-mono text-xs text-zinc-400">Reference {error.digest}</p>}
      <button
        onClick={reset}
        className="h-11 rounded-xl bg-zinc-900 px-5 text-sm font-medium text-white transition hover:bg-zinc-800 dark:bg-white dark:text-zinc-900 dark:hover:bg-zinc-200"
      >
        Try again
      </button>
    </main>
  );
}
