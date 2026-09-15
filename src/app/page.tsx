import { ConsoleTabs } from "@/components/ConsoleTabs";

export default function Home() {
  return (
    <main className="mx-auto flex min-h-dvh w-full max-w-6xl flex-col px-4 pb-8 pt-[max(1.5rem,env(safe-area-inset-top))] sm:px-6 lg:px-8">
      <header className="mb-5 flex items-center gap-3">
        <div className="flex size-9 items-center justify-center rounded-xl bg-zinc-900 text-sm font-bold text-white dark:bg-white dark:text-zinc-900">
          E
        </div>
        <div>
          <h1 className="text-base leading-tight font-semibold">ESP32 Console</h1>
          <p className="text-xs text-zinc-500">Flash and configure your boards</p>
        </div>
      </header>
      <ConsoleTabs />
    </main>
  );
}
