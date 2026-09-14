"use client";

import {
  Button,
  Dialog,
  DialogBackdrop,
  DialogPanel,
  DialogTitle,
  Disclosure,
  DisclosureButton,
  DisclosurePanel,
  Field,
  Label,
  Switch,
} from "@headlessui/react";
import { useEffect, useRef, useState, type ReactNode } from "react";
import { Select } from "@/components/Select";
import { BOARDS, firmwareUrl, type Board, type FirmwareVersion } from "@/lib/boards";
import {
  connect,
  errorMessage,
  findKnownPort,
  flash,
  loadFirmware,
  loadSerialApi,
  release,
  type ChipInfo,
  type SerialApi,
  type Session,
} from "@/lib/esp";

type Phase =
  | "loading"
  | "unsupported"
  | "waiting"
  | "connecting"
  | "ready"
  | "preparing"
  | "flashing"
  | "done"
  | "error";

/** Ignore plug events right after we reset the board (USB re-enumeration). */
const RESET_COOLDOWN_MS = 8000;
const MAX_LOG_LINES = 400;

export function Flasher() {
  const [phase, setPhase] = useState<Phase>("loading");
  const [error, setError] = useState<string | null>(null);
  const [info, setInfo] = useState<ChipInfo | null>(null);
  const [apiKind, setApiKind] = useState<SerialApi["kind"] | null>(null);

  const [board, setBoard] = useState<Board>(BOARDS[0]);
  const [version, setVersion] = useState<FirmwareVersion | null>(
    BOARDS[0].versions[0] ?? null,
  );
  const vendors = [...new Set(BOARDS.map((b) => b.vendor))];
  const [source, setSource] = useState("bundled");
  const [customFile, setCustomFile] = useState<File | null>(null);
  const [fileError, setFileError] = useState<string | null>(null);
  const [flashAddress, setFlashAddress] = useState("0x0");
  const custom = source === "custom";
  const address = /^(0x[0-9a-f]+|[0-9]+)$/i.test(flashAddress.trim()) ? Number(flashAddress.trim()) : NaN;
  const addressError = custom && (!Number.isSafeInteger(address) || address < 0 || address % 4096 !== 0)
    ? "Enter a flash address aligned to 0x1000 (for example 0x0 or 0x10000)." : null;
  const firmwareLabel = custom ? customFile?.name : version ? `v${version.version}` : undefined;
  const [eraseAll, setEraseAll] = useState(false);
  const [confirmOpen, setConfirmOpen] = useState(false);

  const [progress, setProgress] = useState(0);
  const [log, setLog] = useState<string[]>([]);

  const apiRef = useRef<SerialApi | null>(null);
  /** Port of the identified board. The port is closed while idle so the firmware keeps running. */
  const portRef = useRef<SerialPort | null>(null);
  const busyRef = useRef(false);
  const cooldownUntilRef = useRef(0);

  const appendLog = (line: string) => {
    const text = line.trimEnd();
    if (!text) return;
    const timestamp = new Date().toLocaleTimeString("en-GB", { hour12: false });
    setLog((prev) => [...prev, ...text.split(/\r?\n/).map((entry) => `[${timestamp}] ${entry}`)].slice(-MAX_LOG_LINES));
  };

  const captureEsptool = (line: string) => appendLog(`[esptool] ${line}`);

  const forgetBoard = (reason?: string) => {
    portRef.current = null;
    setInfo(null);
    if (reason) appendLog(reason);
  };

  /** Resets the board into its application and ignores the USB re-enumeration that follows. */
  const releaseBoard = async (session: Session) => {
    cooldownUntilRef.current = Date.now() + RESET_COOLDOWN_MS;
    await release(session);
  };

  /** Briefly enters the bootloader to read chip info, then restarts the board. */
  const identify = async (port: SerialPort) => {
    if (busyRef.current) return;
    busyRef.current = true;
    setError(null);
    setPhase("connecting");
    const { usbVendorId, usbProductId } = port.getInfo();
    appendLog(`Connecting to USB ${hex(usbVendorId)}:${hex(usbProductId)} at 921600 baud…`);

    try {
      const session = await connect(port, captureEsptool);
      appendLog(`Chip: ${session.info.description}; MAC: ${session.info.mac}; flash: ${session.info.flashSize}`);
      await releaseBoard(session);
      appendLog("Bootloader released; board restarted.");
      portRef.current = port;
      setInfo(session.info);
      setPhase("ready");
      appendLog(
        `Board detected: ${session.info.chip}, ${session.info.flashSize} flash (USB ${hex(usbVendorId)}:${hex(usbProductId)})`,
      );
    } catch (err) {
      forgetBoard();
      setError(errorMessage(err));
      setPhase("error");
      appendLog(`Detection failed: ${errorMessage(err)}`);

    } finally {
      busyRef.current = false;
    }
  };

  const scan = async ({ force = false } = {}) => {
    const api = apiRef.current;
    if (!api || busyRef.current) return;
    const port = await findKnownPort(api);

    if (!port) {
      if (portRef.current) forgetBoard("Board disconnected.");
      setPhase((p) => (p === "done" || p === "error" ? p : "waiting"));
      return;
    }

    const coolingDown = Date.now() < cooldownUntilRef.current;
    if (portRef.current && (coolingDown || port === portRef.current) && !force) {
      // Same board after our own reset: keep its info, follow the new port object.
      portRef.current = port;
      return;
    }
    if (coolingDown && !force) return;
    await identify(port);
  };

  useEffect(() => {
    let unsubscribe: (() => void) | undefined;
    let cancelled = false;

    loadSerialApi().then((api) => {
      if (cancelled) return;
      if (!api) {
        appendLog("Browser has no supported Web Serial or WebUSB API.");
        setPhase("unsupported");
        return;
      }
      apiRef.current = api;
      setApiKind(api.kind);
      appendLog(`USB API: ${api.kind === "native" ? "Web Serial" : "WebUSB polyfill"}. Waiting for an authorized device.`);
      setPhase("waiting");
      unsubscribe = api.onChange(() => void scan());
      void scan();
    });

    return () => {
      cancelled = true;
      unsubscribe?.();
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const selectBoard = (next: Board) => {
    if (next.id === board.id) return;
    setBoard(next);
    setVersion(next.versions[0] ?? null);
    appendLog(`Selected board: ${next.vendor} / ${next.name}`);
  };

  const requestBoard = async () => {
    const api = apiRef.current;
    if (!api) return;
    try {
      const port = await api.requestPort();
      await identify(port);
    } catch (err) {
      // User closed the picker without choosing a device.
      if (err instanceof DOMException && err.name === "NotFoundError") return;
      setError(errorMessage(err));
      setPhase("error");
      appendLog(`USB device selection failed: ${errorMessage(err)}`);
    }
  };

  const startFlash = async () => {
    setConfirmOpen(false);
    const port = portRef.current;
    if (!port || (custom ? !customFile || !!fileError || !!addressError : !version) || busyRef.current) return;
    const startedAt = performance.now();

    busyRef.current = true;
    setError(null);
    const wakeLock = await navigator.wakeLock?.request("screen").catch(() => null);
    let session: Session | null = null;

    try {
      setPhase("preparing");
      setProgress(0);
      appendLog(`Preparing ${custom ? "local file" : `${board.vendor} / ${board.name}`}: ${firmwareLabel}`);
      const image = custom
        ? new Uint8Array(await customFile!.arrayBuffer())
        : await loadFirmware(firmwareUrl(board, version!), version!.sha256, setProgress);
      if (custom) {
        const digest = await crypto.subtle.digest("SHA-256", image);
        appendLog(`Local file SHA-256: ${Array.from(new Uint8Array(digest), (b) => b.toString(16).padStart(2, "0")).join("")}`);
      } else {
        appendLog(`SHA-256 verified: ${version!.sha256}`);
        appendLog(`Firmware built: ${version!.builtAt}`);
      }
      appendLog(`Image: ${image.byteLength.toLocaleString()} bytes; address: 0x${(custom ? address : 0).toString(16)}; full erase: ${eraseAll ? "yes" : "no"}`);
      session = await connect(port, captureEsptool);
      setInfo(session.info);
      const mismatch = custom ? null : checkCompatibility(board, session.info);
      if (mismatch) throw new Error(mismatch);
      appendLog(`Connected chip: ${session.info.description}; flash: ${session.info.flashSize}; MAC: ${session.info.mac}`);
      if (!custom && !eraseAll) {
        for (const region of board.preserve ?? []) appendLog(`Preserving ${region.name}: 0x${region.offset.toString(16)} (${region.size} bytes)`);
      }
      setPhase("flashing");
      setProgress(0);
      let lastPercent = -10;
      await flash(session, image, {
        eraseAll,
        address: custom ? address : 0,
        preserve: custom ? undefined : board.preserve,
        onProgress: (ratio) => {
          setProgress(ratio);
          const percent = Math.floor(ratio * 100);
          if (percent >= lastPercent + 10 || percent === 100 && lastPercent !== 100) {
            appendLog(`Writing flash: ${percent}%`);
            lastPercent = percent;
          }
        },
      });
      appendLog(`Flash completed in ${((performance.now() - startedAt) / 1000).toFixed(1)} seconds.`);

      await releaseBoard(session);
      appendLog("Board restarted; USB port released.");
      session = null;
      setPhase("done");
    } catch (err) {
      setError(errorMessage(err));
      setPhase("error");
      appendLog(`Flashing failed: ${errorMessage(err)}`);

      if (session) await releaseBoard(session);
    } finally {
      busyRef.current = false;
      await wakeLock?.release().catch(() => {});
    }
  };

  const reset = () => {
    setError(null);
    setProgress(0);
    setPhase(portRef.current ? "ready" : "waiting");
    if (!portRef.current) void scan({ force: true });
  };

  const redetect = () => {
    forgetBoard();
    setPhase("waiting");
    void scan({ force: true });
  };

  const working = phase === "preparing" || phase === "flashing";

  useEffect(() => {
    if (!working) return;
    const guard = (e: BeforeUnloadEvent) => e.preventDefault();
    window.addEventListener("beforeunload", guard);
    return () => window.removeEventListener("beforeunload", guard);
  }, [working]);

  const incompatibility = custom
    ? fileError ?? addressError ?? (info?.flashSizeMb && customFile && address + customFile.size > info.flashSizeMb * 1024 * 1024 ? "File exceeds the connected board’s flash capacity." : null)
    : info ? checkCompatibility(board, info) : null;
  const canFlash =
    phase === "ready" && !!info && (custom ? !!customFile : !!version) && !incompatibility;

  return (
    <div className="flex flex-1 flex-col gap-4">
      <DeviceCard
        phase={phase}
        info={info}
        apiKind={apiKind}
        error={phase === "error" ? error : null}
        onRequest={requestBoard}
        onRedetect={redetect}
        onRetry={reset}
      />

      <section className="space-y-4 rounded-2xl border border-zinc-200 bg-white p-4 dark:border-zinc-800 dark:bg-zinc-900/60">
        <h2 className="text-sm font-semibold">Firmware</h2>

        <Select
          label="Firmware source"
          value={source}
          options={["bundled", "custom"]}
          onChange={setSource}
          getKey={(s) => s}
          renderValue={(s) => s === "bundled" ? "Built-in firmware" : "Upload .bin file"}
          disabled={working}
        />

        {!custom && <>
        <Select
          label="Manufacturer"
          value={board.vendor}
          options={vendors}
          onChange={(vendor) => selectBoard(BOARDS.find((b) => b.vendor === vendor)!)}
          getKey={(vendor) => vendor}
          renderValue={(vendor) => vendor}
          disabled={working}
        />
        <Select
          label="Board model"
          value={board}
          options={BOARDS.filter((b) => b.vendor === board.vendor)}
          onChange={selectBoard}
          getKey={(b) => b.id}
          disabled={working}
          renderValue={(b) => b.name}
        />

        <Select
          label="Version"
          value={version}
          options={board.versions}
          onChange={setVersion}
          getKey={(v) => v.version}
          disabled={working || board.versions.length === 0}
          placeholder="No firmware built"
          renderValue={(v) => (
            <span className="flex items-center gap-2">
              <span className="font-medium">v{v.version}</span>
              {v === board.versions[0] && <Badge>Latest</Badge>}
              <span className="text-xs text-zinc-400">{formatBytes(v.size)}</span>
            </span>
          )}
          renderOption={(v) => (
            <div className="flex items-center justify-between gap-3">
              <span className="flex items-center gap-2">
                <span className="font-medium">v{v.version}</span>
                {v === board.versions[0] && <Badge>Latest</Badge>}
              </span>
              <span className="text-xs text-zinc-400">{formatDate(v.builtAt)}</span>
            </div>
          )}
        />

        </>}

        {custom && (
          <div className="space-y-3">
            <label className="block space-y-1.5 text-xs font-medium text-zinc-500">
              <span>Firmware file (.bin)</span>
              <input
                type="file"
                accept=".bin,application/octet-stream"
                disabled={working}
                className="block w-full rounded-xl border border-zinc-200 p-3 text-sm file:mr-3 file:rounded-lg file:border-0 file:bg-zinc-100 file:px-3 file:py-2 dark:border-zinc-800 dark:file:bg-zinc-800"
                onChange={(event) => {
                  const file = event.target.files?.[0] ?? null;
                  setCustomFile(file);
                  const issue = file && (!file.name.toLowerCase().endsWith(".bin") || file.size === 0)
                    ? "Select a non-empty .bin firmware file." : null;
                  setFileError(issue);
                  if (file) appendLog(issue ? `File rejected: ${issue}` : `Local file selected: ${file.name} (${file.size.toLocaleString()} bytes)`);
                }}
              />
            </label>
            {customFile && <p className="break-all text-xs text-zinc-500">{customFile.name} · {formatBytes(customFile.size)}</p>}
            <label className="block space-y-1.5 text-xs font-medium text-zinc-500">
              <span>Flash address</span>
              <input value={flashAddress} onChange={(event) => setFlashAddress(event.target.value)} disabled={working} spellCheck={false} className="h-11 w-full rounded-xl border border-zinc-200 px-3 font-mono text-sm dark:border-zinc-800 dark:bg-zinc-900" />
            </label>
            <p className="text-xs text-zinc-500">The file stays in your browser. Use firmware built for your connected board. Merged images usually start at 0x0; application images use the address specified by their build (often 0x10000). Bootloader and partition table must already be installed for an application-only image.</p>
          </div>
        )}

        <Field
          disabled={working}
          className="flex items-center justify-between gap-4 data-disabled:opacity-50"
        >
          <div>
            <Label className="text-sm">Erase flash first</Label>
            <p className="text-xs text-zinc-500">
              {eraseAll ? "All flash contents and saved settings will be lost" : custom ? "Only sectors covered by the file are overwritten" : "Saved settings are kept"}
            </p>
          </div>
          <Switch
            checked={eraseAll}
            onChange={setEraseAll}
            className="group relative inline-flex h-7 w-12 shrink-0 cursor-pointer rounded-full bg-zinc-200 p-0.5 transition data-checked:bg-zinc-900 dark:bg-zinc-700 dark:data-checked:bg-white"
          >
            <span className="size-6 translate-x-0 rounded-full bg-white shadow transition group-data-checked:translate-x-5 dark:group-data-checked:bg-zinc-900" />
          </Switch>
        </Field>
      </section>

      <LogPanel lines={log} onClear={() => setLog([])} />

      <div className="sticky bottom-0 -mx-4 mt-auto border-t border-zinc-200/70 bg-zinc-50/90 px-4 pt-3 pb-[max(1rem,env(safe-area-inset-bottom))] backdrop-blur dark:border-zinc-800/70 dark:bg-zinc-950/90">
        <ActionArea
          phase={phase}
          progress={progress}
          canFlash={canFlash}
          hint={incompatibility}
          version={firmwareLabel}
          onFlash={() => setConfirmOpen(true)}
          onAgain={reset}
        />
      </div>

      <Dialog open={confirmOpen} onClose={setConfirmOpen} className="relative z-50">
        <DialogBackdrop
          transition
          className="fixed inset-0 bg-black/40 transition duration-200 data-closed:opacity-0"
        />
        <div className="fixed inset-0 flex items-end justify-center p-4 sm:items-center">
          <DialogPanel
            transition
            className="w-full max-w-sm space-y-4 rounded-2xl bg-white p-5 shadow-xl transition duration-200 ease-out data-closed:translate-y-4 data-closed:opacity-0 dark:bg-zinc-900"
          >
            <DialogTitle className="text-base font-semibold">Flash firmware?</DialogTitle>
            <dl className="space-y-1.5 rounded-xl bg-zinc-50 p-3 text-sm dark:bg-zinc-800/60">
              <Row label="Board" value={custom ? info?.chip ?? "Connected device" : `${board.vendor} / ${board.name}`} />
              <Row label="Firmware" value={firmwareLabel ?? "—"} />
              <Row label="Address" value={custom ? `0x${address.toString(16)}` : "0x0"} />
              <Row label="Erase flash" value={eraseAll ? "Yes" : "No"} />
            </dl>
            <p className="text-sm text-zinc-500">
              The current firmware will be overwritten. Keep the board connected
              and this page open until flashing finishes.
            </p>
            <div className="grid grid-cols-2 gap-2">
              <Button
                onClick={() => setConfirmOpen(false)}
                className="h-11 rounded-xl border border-zinc-200 text-sm font-medium data-hover:bg-zinc-50 dark:border-zinc-700 dark:data-hover:bg-zinc-800"
              >
                Cancel
              </Button>
              <Button
                onClick={startFlash}
                className="h-11 rounded-xl bg-zinc-900 text-sm font-medium text-white data-hover:bg-zinc-800 dark:bg-white dark:text-zinc-900 dark:data-hover:bg-zinc-200"
              >
                Flash
              </Button>
            </div>
          </DialogPanel>
        </div>
      </Dialog>
    </div>
  );
}

function DeviceCard({
  phase,
  info,
  apiKind,
  error,
  onRequest,
  onRedetect,
  onRetry,
}: {
  phase: Phase;
  info: ChipInfo | null;
  apiKind: SerialApi["kind"] | null;
  error: string | null;
  onRequest: () => void;
  onRedetect: () => void;
  onRetry: () => void;
}) {
  const status = {
    loading: { dot: "bg-zinc-300", title: "Checking browser…" },
    unsupported: { dot: "bg-red-500", title: "USB not supported" },
    waiting: { dot: "bg-zinc-300 animate-pulse", title: "Waiting for board" },
    connecting: { dot: "bg-amber-400 animate-pulse", title: "Connecting…" },
    ready: { dot: "bg-emerald-500", title: info?.chip ?? "Connected" },
    preparing: { dot: "bg-emerald-500", title: info?.chip ?? "Connected" },
    flashing: { dot: "bg-blue-500 animate-pulse", title: "Flashing…" },
    done: { dot: "bg-emerald-500", title: "Board restarted" },
    error: { dot: "bg-red-500", title: "Something went wrong" },
  }[phase];

  return (
    <section className="rounded-2xl border border-zinc-200 bg-white p-4 dark:border-zinc-800 dark:bg-zinc-900/60">
      <div className="flex items-center gap-3">
        <div className="flex size-10 shrink-0 items-center justify-center rounded-xl bg-zinc-100 dark:bg-zinc-800">
          {phase === "connecting" ? <Spinner /> : <UsbIcon />}
        </div>
        <div className="min-w-0 flex-1">
          <div className="flex items-center gap-2">
            <span className={`size-2 shrink-0 rounded-full ${status.dot}`} />
            <h2 className="truncate text-sm font-semibold">{status.title}</h2>
          </div>
          <p className="truncate text-xs text-zinc-500">
            {info
              ? `${info.flashSize} flash · ${info.mac}`
              : phase === "unsupported"
                ? "Use Chrome or Edge on desktop or Android"
                : phase === "connecting"
                  ? "Entering bootloader"
                  : phase === "done"
                    ? "Firmware is running"
                    : "Plug in an ESP32 via USB"}
          </p>
        </div>
        {info && (phase === "ready" || phase === "error" || phase === "done") && (
          <Button
            onClick={onRedetect}
            className="rounded-lg px-2.5 py-1.5 text-xs font-medium text-zinc-500 data-hover:bg-zinc-100 dark:data-hover:bg-zinc-800"
          >
            Re-detect
          </Button>
        )}
      </div>

      {info && (
        <p className="mt-3 truncate rounded-lg bg-zinc-50 px-3 py-2 font-mono text-[11px] text-zinc-500 dark:bg-zinc-800/60">
          {info.description}
        </p>
      )}

      {error && (
        <div className="mt-3 space-y-2 rounded-xl bg-red-50 p-3 text-sm text-red-700 dark:bg-red-950/40 dark:text-red-300">
          <p className="break-words">{error}</p>
          <p className="text-xs opacity-80">
            If it keeps failing, hold BOOT, tap RESET, release BOOT and retry.
          </p>
          <Button onClick={onRetry} className="text-xs font-semibold underline underline-offset-2">
            Retry
          </Button>
        </div>
      )}

      {(phase === "waiting" || (phase === "error" && !info)) && (
        <div className="mt-3 space-y-2">
          <Button
            onClick={onRequest}
            className="h-11 w-full rounded-xl border border-zinc-200 text-sm font-medium data-active:scale-[.99] data-hover:bg-zinc-50 dark:border-zinc-700 dark:data-hover:bg-zinc-800"
          >
            Select USB device
          </Button>
          <p className="text-center text-[11px] text-zinc-400">
            Allow access once — afterwards boards are detected automatically.
            {apiKind === "webusb" && " Android: native-USB boards only."}
          </p>
        </div>
      )}
    </section>
  );
}

function ActionArea({
  phase,
  progress,
  canFlash,
  hint,
  version,
  onFlash,
  onAgain,
}: {
  phase: Phase;
  progress: number;
  canFlash: boolean;
  hint: string | null;
  version?: string;
  onFlash: () => void;
  onAgain: () => void;
}) {
  if (phase === "preparing" || phase === "flashing") {
    const percent = Math.round(progress * 100);
    return (
      <div className="space-y-2" role="status" aria-live="polite">
        <div className="flex items-center justify-between text-sm">
          <span className="flex items-center gap-2 font-medium">
            <Spinner />
            {phase === "preparing" ? "Preparing firmware" : "Writing to flash"}
          </span>
          <span className="tabular-nums text-zinc-500">{percent}%</span>
        </div>
        <div className="h-2 overflow-hidden rounded-full bg-zinc-200 dark:bg-zinc-800">
          <div
            className={`h-full rounded-full transition-[width] duration-300 ${
              phase === "preparing" ? "bg-zinc-400" : "bg-blue-500"
            }`}
            style={{ width: `${percent}%` }}
          />
        </div>
        <p className="text-center text-[11px] text-zinc-400">
          Do not unplug the board or close this page
        </p>
      </div>
    );
  }

  if (phase === "done") {
    return (
      <div className="space-y-2">
        <p className="text-center text-sm font-medium text-emerald-600 dark:text-emerald-400">
          ✓ {version} flashed successfully
        </p>
        <Button
          onClick={onAgain}
          className="h-12 w-full rounded-xl bg-zinc-900 text-sm font-semibold text-white data-hover:bg-zinc-800 dark:bg-white dark:text-zinc-900"
        >
          Flash another board
        </Button>
      </div>
    );
  }

  return (
    <div className="space-y-2">
      {hint && <p className="text-center text-xs text-red-600 dark:text-red-400">{hint}</p>}
      <Button
        onClick={onFlash}
        disabled={!canFlash}
        className="h-12 w-full rounded-xl bg-zinc-900 text-sm font-semibold text-white transition data-active:scale-[.99] data-disabled:cursor-not-allowed data-disabled:opacity-30 data-hover:bg-zinc-800 dark:bg-white dark:text-zinc-900 dark:data-hover:bg-zinc-200"
      >
        {version ? `Flash ${version}` : "Flash firmware"}
      </Button>
    </div>
  );
}

function LogPanel({ lines, onClear }: { lines: string[]; onClear: () => void }) {
  const outputRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    const output = outputRef.current;
    if (output) output.scrollTop = output.scrollHeight;
  }, [lines]);

  return (
    <Disclosure
      as="section"
      defaultOpen
      className="rounded-2xl border border-zinc-200 bg-white dark:border-zinc-800 dark:bg-zinc-900/60"
    >
      <div className="flex items-center">
        <DisclosureButton className="group flex flex-1 items-center gap-2 p-4 text-left text-sm font-semibold">
          <svg viewBox="0 0 20 20" fill="currentColor" className="size-4 text-zinc-400 transition group-data-open:rotate-90" aria-hidden>
            <path d="M7.21 14.77a.75.75 0 0 1 .02-1.06L11.17 10 7.23 6.29a.75.75 0 1 1 1.04-1.08l4.5 4.25a.75.75 0 0 1 0 1.08l-4.5 4.25a.75.75 0 0 1-1.06-.02Z" />
          </svg>
          Log
          <span className="font-normal text-zinc-400">{lines.length}</span>
        </DisclosureButton>
        {lines.length > 0 && (
          <Button onClick={onClear} className="mr-3 rounded-lg px-2.5 py-1.5 text-xs text-zinc-500 data-hover:bg-zinc-100 dark:data-hover:bg-zinc-800">
            Clear
          </Button>
        )}
      </div>
      <DisclosurePanel ref={outputRef} className="max-h-56 overflow-y-auto border-t border-zinc-200 px-4 py-3 font-mono text-[11px] leading-relaxed text-zinc-500 dark:border-zinc-800">
        {lines.length === 0 ? (
          <p className="text-zinc-400">No output yet.</p>
        ) : (
          lines.map((line, i) => (
            <p key={i} className="break-all whitespace-pre-wrap">
              {line}
            </p>
          ))
        )}
      </DisclosurePanel>
    </Disclosure>
  );
}

function checkCompatibility(board: Board, info: ChipInfo) {
  if (info.chip !== board.chip) {
    return `This firmware needs ${board.chip}, connected board is ${info.chip}.`;
  }
  if (info.flashSizeMb && info.flashSizeMb < board.flashSizeMb) {
    return `This firmware needs ${board.flashSizeMb}MB flash, board has ${info.flashSize}.`;
  }
  return null;
}

function Row({ label, value }: { label: string; value: string }) {
  return (
    <div className="flex justify-between gap-4">
      <dt className="text-zinc-500">{label}</dt>
      <dd className="min-w-0 break-all text-right font-medium">{value}</dd>
    </div>
  );
}

function Badge({ children }: { children: ReactNode }) {
  return (
    <span className="rounded-md bg-zinc-100 px-1.5 py-0.5 text-[10px] font-medium text-zinc-500 dark:bg-zinc-800">
      {children}
    </span>
  );
}

function Spinner() {
  return (
    <svg viewBox="0 0 24 24" className="size-4 animate-spin" fill="none" aria-hidden>
      <circle cx="12" cy="12" r="9" stroke="currentColor" strokeOpacity=".2" strokeWidth="3" />
      <path d="M21 12a9 9 0 0 0-9-9" stroke="currentColor" strokeWidth="3" strokeLinecap="round" />
    </svg>
  );
}

function UsbIcon() {
  return (
    <svg viewBox="0 0 24 24" className="size-5 text-zinc-500" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round" aria-hidden>
      <path d="M12 3v14M12 3l-2.5 3M12 3l2.5 3M7 9v2.5a2 2 0 0 0 2 2h3M17 8v4a2 2 0 0 1-2 2h-3" />
      <circle cx="12" cy="19" r="2" />
      <rect x="15.5" y="6" width="3" height="2" />
      <circle cx="7" cy="8" r="1" />
    </svg>
  );
}

function hex(n?: number) {
  return n === undefined ? "????" : n.toString(16).padStart(4, "0");
}

function formatBytes(bytes: number) {
  return bytes < 1024 * 1024
    ? `${Math.round(bytes / 1024)} KB`
    : `${(bytes / 1024 / 1024).toFixed(1)} MB`;
}

function formatDate(iso: string) {
  return new Date(iso).toLocaleDateString("en", {
    year: "numeric",
    month: "short",
    day: "numeric",
  });
}
