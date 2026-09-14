"use client";

import { Button } from "@headlessui/react";
import { signIn } from "next-auth/react";
import { cardClass } from "@/components/ui";

export function AuthCard() {
  return (
    <div className={cardClass + " space-y-4 p-5 text-center"}>
      <div className="space-y-1">
        <h2 className="text-base font-semibold">Sign in</h2>
        <p className="text-sm text-zinc-500">Your devices are linked to your Google account and can be managed from any browser.</p>
      </div>
      <Button
        onClick={() => signIn("google")}
        className="inline-flex h-11 w-full items-center justify-center gap-2.5 rounded-xl border border-zinc-200 text-sm font-medium transition data-active:scale-[.99] data-hover:bg-zinc-50 dark:border-zinc-700 dark:data-hover:bg-zinc-800"
      >
        <GoogleIcon />
        Sign in with Google
      </Button>
    </div>
  );
}

function GoogleIcon() {
  return (
    <svg viewBox="0 0 20 20" className="size-4.5" aria-hidden>
      <path
        d="M19.6 10.23c0-.68-.06-1.32-.17-1.94H10v3.67h5.38a4.6 4.6 0 0 1-2 3.02v2.5h3.23c1.9-1.75 2.99-4.32 2.99-7.25Z"
        fill="#4285F4"
      />
      <path
        d="M10 20c2.7 0 4.96-.89 6.62-2.42l-3.23-2.5c-.9.6-2.05.95-3.39.95-2.6 0-4.8-1.76-5.59-4.12H1.07v2.59A10 10 0 0 0 10 20Z"
        fill="#34A853"
      />
      <path d="M4.41 11.9a6 6 0 0 1 0-3.8V5.51H1.07a10 10 0 0 0 0 8.98l3.34-2.6Z" fill="#FBBC05" />
      <path
        d="M10 3.98c1.47 0 2.79.5 3.83 1.5l2.87-2.87C14.95.99 12.7 0 10 0A10 10 0 0 0 1.07 5.51l3.34 2.6C5.2 5.74 7.4 3.98 10 3.98Z"
        fill="#EA4335"
      />
    </svg>
  );
}
