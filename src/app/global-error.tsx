"use client";

/** Last resort: the root layout itself failed, so this renders its own html/body. */
export default function GlobalError({ error, reset }: { error: Error & { digest?: string }; reset: () => void }) {
  return (
    <html lang="en" className="h-full antialiased">
      <body className="min-h-full bg-zinc-50 text-zinc-900 dark:bg-zinc-950 dark:text-zinc-100">
        <main className="mx-auto flex min-h-dvh w-full max-w-md flex-col items-center justify-center gap-4 px-6 text-center">
          <h1 className="text-base font-semibold">The console failed to start</h1>
          <p className="text-sm text-zinc-500">Reload the page. If it keeps failing, check the server logs.</p>
          {error.digest && <p className="font-mono text-xs text-zinc-400">Reference {error.digest}</p>}
          <button
            onClick={reset}
            className="h-11 rounded-xl bg-zinc-900 px-5 text-sm font-medium text-white transition hover:bg-zinc-800 dark:bg-white dark:text-zinc-900 dark:hover:bg-zinc-200"
          >
            Reload
          </button>
        </main>
      </body>
    </html>
  );
}
