import Link from "next/link";

export default function NotFound() {
  return (
    <main className="mx-auto flex min-h-dvh w-full max-w-md flex-col items-center justify-center gap-4 px-6 text-center">
      <h1 className="text-base font-semibold">Page not found</h1>
      <p className="text-sm text-zinc-500">This address is not part of the console.</p>
      <Link
        href="/"
        className="flex h-11 items-center rounded-xl bg-zinc-900 px-5 text-sm font-medium text-white transition hover:bg-zinc-800 dark:bg-white dark:text-zinc-900 dark:hover:bg-zinc-200"
      >
        Back to the console
      </Link>
    </main>
  );
}
