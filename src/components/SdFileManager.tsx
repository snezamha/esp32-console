"use client";

import { Button } from "@headlessui/react";
import { useCallback, useEffect, useRef, useState } from "react";
import { ConfirmDialog, ErrorText, Sheet, inputClass, secondaryButton } from "@/components/ui";
import { api, compareVersions, deviceName } from "@/lib/device-client";
import { SD_MOUNT_FIRMWARE, type FileCommandType, type FileJob, type FileProgress, type PublicDevice, type SdEntry } from "@/lib/device-types";
import { errorMessage } from "@/lib/esp";
import { connectSerialLink, type SerialLink } from "@/lib/serial-link";

const MAX_UPLOAD = 4 * 1024 * 1024;
const MAX_USB_TRANSFER = 512 * 1024 * 1024;
const JOB_TIMEOUT_MS = 3.5 * 60 * 1000;

export function formatBytes(bytes: number) {
  if (bytes >= 1024 ** 3) return `${(bytes / 1024 ** 3).toFixed(1)} GB`;
  if (bytes >= 1024 ** 2) return `${(bytes / 1024 ** 2).toFixed(1)} MB`;
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${bytes} B`;
}

const join = (folder: string, name: string) => (folder === "/" ? `/${name}` : `${folder}/${name}`);
const parent = (path: string) => path.slice(0, path.lastIndexOf("/")) || "/";

/** Where a file operation is: the browser upload, the queue until the board's next check-in, or the board's own work. */
export type JobStage = { type: FileCommandType; stage: "upload" | "queued" | "board" | "usb"; progress: FileProgress | null; startedAt: number };

/** POST with upload progress; `fetch` cannot report how much of a request body was sent. */
function postWithProgress(url: string, form: FormData, onProgress: (progress: FileProgress) => void) {
  return new Promise<{ ok: boolean; status: number; data: Record<string, string> }>((resolve, reject) => {
    const request = new XMLHttpRequest();
    request.open("POST", url);
    request.upload.onprogress = (event) => { if (event.lengthComputable) onProgress({ done: event.loaded, total: event.total }); };
    request.onload = () => {
      let data = {};
      try { data = JSON.parse(request.responseText); } catch {}
      resolve({ ok: request.status >= 200 && request.status < 300, status: request.status, data });
    };
    request.onerror = () => reject(new Error("Could not reach the console."));
    request.send(form);
  });
}

/**
 * Starts a board operation and waits for its result. The board reaches the console on its own, so
 * every step is a queued command that the next board check-in picks up. `onStage` receives each
 * stage and the progress the board reports while it works.
 */
export async function runFileJob(device: PublicDevice, body: object | FormData, onStage?: (stage: JobStage) => void): Promise<FileJob> {
  const url = `/api/devices/${device.id}/files`;
  const type: FileCommandType = body instanceof FormData ? "sd_upload" : (body as { type: FileCommandType }).type;
  const startedAt = Date.now();
  const report = (stage: JobStage["stage"], progress: FileProgress | null) => onStage?.({ type, stage, progress, startedAt });
  let started: Record<string, string>;
  if (body instanceof FormData) {
    report("upload", null);
    const response = await postWithProgress(url, body, (progress) => report("upload", progress));
    started = response.data;
    if (!response.ok) throw new Error(started.error ?? `Request failed (${response.status})`);
  } else {
    const response = await fetch(url, { method: "POST", cache: "no-store", body: JSON.stringify(body), headers: { "Content-Type": "application/json" } });
    started = await response.json().catch(() => ({}));
    if (!response.ok) throw new Error(started.error ?? `Request failed (${response.status})`);
  }
  report("queued", null);
  const deadline = Date.now() + JOB_TIMEOUT_MS;
  for (;;) {
    await new Promise((resolve) => setTimeout(resolve, 600));
    const { job } = await api<{ job: FileJob }>(`/api/devices/${device.id}/files/${started.job}`);
    if (job.status === "done") return job;
    if (job.status === "failed") throw new Error(job.result || "The board could not complete the operation.");
    report(job.status === "queued" ? "queued" : "board", job.progress ?? null);
    if (Date.now() > deadline) throw new Error("The board did not answer in time. Check that it is online.");
  }
}

const BOARD_WORK: Partial<Record<FileCommandType, string>> = {
  sd_upload: "Board is saving the file",
  sd_download: "Board is sending the file",
  sd_format: "Board is formatting the card",
  sd_list: "Board is reading the folder",
};

/** Stage, measured progress and elapsed time of a running file operation. */
export function JobProgress({ label, stage }: { label: string; stage: JobStage | null }) {
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    const timer = window.setInterval(() => setNow(Date.now()), 500);
    return () => window.clearInterval(timer);
  }, []);
  const progress = stage?.progress && stage.progress.total > 0 ? stage.progress : null;
  const percent = progress ? Math.min(100, Math.floor((progress.done / progress.total) * 100)) : null;
  const detail = !stage ? "Starting…"
    : stage.stage === "upload" ? "Sending the file to the console"
    : stage.stage === "queued" ? "Waiting for the board to pick up the request"
    : `${BOARD_WORK[stage.type] ?? "Board is working"}${stage.stage === "usb" ? " · over USB" : ""}`;
  const amount = progress && stage?.type !== "sd_format" ? ` · ${formatBytes(progress.done)} of ${formatBytes(progress.total)}` : "";
  const elapsed = stage ? Math.max(0, Math.floor((now - stage.startedAt) / 1000)) : 0;
  return (
    <div role="status" aria-live="polite" className="space-y-1.5">
      <div className="flex justify-between gap-3 text-sm">
        <span className="min-w-0 truncate font-medium">{label}</span>
        <span className="shrink-0 tabular-nums text-zinc-500">{percent === null ? `${elapsed}s` : `${percent}%`}</span>
      </div>
      <div className="h-2 overflow-hidden rounded-full bg-zinc-200 dark:bg-zinc-800">
        {percent === null
          ? <div className="h-full w-full animate-pulse rounded-full bg-blue-500/40" />
          : <div className="h-full rounded-full bg-blue-500 transition-[width] duration-300" style={{ width: `${percent}%` }} />}
      </div>
      <p className="text-xs text-zinc-500">{detail}{amount}{percent !== null ? ` · ${elapsed}s` : ""}</p>
    </div>
  );
}

/** Browse, upload, download, rename and delete files on the board's SD card. */
export function SdFileManager({ device, onClose }: { device: PublicDevice; onClose: () => void }) {
  const [path, setPath] = useState("/");
  const [entries, setEntries] = useState<SdEntry[] | null>(null);
  const [note, setNote] = useState("");
  const [busy, setBusy] = useState<string | null>(null);
  const [stage, setStage] = useState<JobStage | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [newFolder, setNewFolder] = useState<string | null>(null);
  const [renaming, setRenaming] = useState<{ entry: SdEntry; name: string } | null>(null);
  const [deleting, setDeleting] = useState<SdEntry | null>(null);
  const [mounted, setMounted] = useState(device.sdCard?.mounted ?? false);
  const [link, setLink] = useState<SerialLink | null>(null);
  // The silent USB attempt runs first, so the first listing already takes the fast path.
  const [probing, setProbing] = useState(true);
  const [usbCard, setUsbCard] = useState<{ total: number; used: number } | null>(null);
  const upload = useRef<HTMLInputElement | null>(null);
  const loaded = useRef(false);
  const linkRef = useRef<SerialLink | null>(null);
  const supportsMount = compareVersions(device.firmware, SD_MOUNT_FIRMWARE) >= 0;

  const run = useCallback((body: object | FormData) => runFileJob(device, body, setStage), [device]);

  const attach = useCallback((opened: SerialLink) => {
    linkRef.current = opened;
    opened.onClose = () => {
      if (linkRef.current === opened) linkRef.current = null;
      setLink((current) => (current === opened ? null : current));
      setUsbCard(null);
    };
    setLink(opened);
  }, []);

  useEffect(() => {
    let cancelled = false;
    void connectSerialLink(device.mac, false)
      .catch(() => null)
      .then((opened) => {
        if (cancelled) return void opened?.close();
        if (opened) attach(opened);
        setProbing(false);
      });
    return () => {
      cancelled = true;
      void linkRef.current?.close();
      linkRef.current = null;
    };
  }, [attach, device.mac]);

  /** Reports an operation over USB to the progress bar. */
  const usbStage = useCallback((type: FileCommandType) => {
    const startedAt = Date.now();
    setStage({ type, stage: "usb", progress: null, startedAt });
    return (progress: FileProgress) => setStage({ type, stage: "usb", progress, startedAt });
  }, []);

  const perform = useCallback(async (label: string, action: () => Promise<void>) => {
    setBusy(label);
    setStage(null);
    setError(null);
    try {
      await action();
    } catch (err) {
      setError(errorMessage(err));
    } finally {
      setBusy(null);
      setStage(null);
    }
  }, []);

  const listFolder = useCallback(async (folder: string) => {
    const usb = linkRef.current;
    if (usb) {
      usbStage("sd_list");
      const listing = await usb.list(folder);
      setUsbCard({ total: listing.total, used: listing.used });
      return { entries: listing.entries, note: `${listing.entries.length} entries${listing.truncated ? " (first 2000 shown)" : ""}` };
    }
    const job = await run({ type: "sd_list", path: folder });
    return { entries: job.entries ?? [], note: job.result };
  }, [run, usbStage]);

  const loadFolder = useCallback(async (folder: string) => {
    const listing = await listFolder(folder);
    setPath(folder);
    setEntries(listing.entries);
    setNote(listing.note);
    setMounted(true);
  }, [listFolder]);

  /** Runs a simple change over USB when linked, otherwise through the console. */
  const change = useCallback(async (type: "sd_mkdir" | "sd_rename" | "sd_delete" | "sd_mount" | "sd_unmount", target = "/", to = "") => {
    const usb = linkRef.current;
    if (!usb) return (await run({ type, path: target, ...(to ? { to } : {}) })).result;
    usbStage(type);
    const reply = type === "sd_mkdir" ? await usb.mkdir(target)
      : type === "sd_rename" ? await usb.rename(target, to)
      : type === "sd_delete" ? await usb.remove(target)
      : type === "sd_mount" ? await usb.mount()
      : await usb.unmount();
    return String(reply.message ?? (type === "sd_mount" ? "SD card mounted" : type === "sd_unmount" ? "SD card unmounted" : "Done"));
  }, [run, usbStage]);

  const connectUsb = () => void perform("Connecting over USB…", async () => {
    try {
      const opened = await connectSerialLink(device.mac, true);
      if (opened) {
        void linkRef.current?.close();
        attach(opened);
        linkRef.current = opened;
        await loadFolder(path);
      }
    } catch (err) {
      if (err instanceof DOMException && err.name === "NotFoundError") return;
      throw err;
    }
  });

  const open = useCallback((folder: string) => perform("Opening folder…", () => loadFolder(folder)), [loadFolder, perform]);

  useEffect(() => {
    if (loaded.current || probing) return;
    // New firmware exposes an explicit mount action. Do not trap a card that has not been checked
    // yet behind a multi-minute automatic listing job; let the user mount it first. Over USB the
    // listing answers at once, so it simply runs.
    if (supportsMount && !mounted && !link) return;
    loaded.current = true;
    const timer = window.setTimeout(() => void open("/"), 0);
    return () => window.clearTimeout(timer);
  }, [link, mounted, open, probing, supportsMount]);

  const validName = (name: string) => name.trim() && !/[\\/\x00-\x1f]/.test(name) && name !== "." && name !== "..";
  const card = device.sdCard;

  return (
    <Sheet
      open
      wide
      onClose={onClose}
      title={`SD card · ${deviceName(device)}`}
      subtitle={usbCard ? `${formatBytes(Math.max(0, usbCard.total - usbCard.used))} free of ${formatBytes(usbCard.total)} · connected over USB` : mounted ? (card?.mounted ? `${formatBytes(card.free)} free of ${formatBytes(card.total)}` : "Card mounted; capacity will update on the next board check-in.") : "The board has not mounted a card."}
    >
      {link ? (
        <p className="rounded-xl bg-emerald-50 p-3 text-xs text-emerald-700 dark:bg-emerald-950 dark:text-emerald-300">Connected over USB: operations go straight to the board. <Button onClick={() => void link.close()} disabled={!!busy} className="font-medium underline underline-offset-2 data-disabled:opacity-50">Disconnect</Button></p>
      ) : !probing && (
        <p className="rounded-xl bg-zinc-50 p-3 text-xs text-zinc-600 dark:bg-zinc-800/60 dark:text-zinc-400">Operations go through the console and take a few seconds each. Plug the board into this computer for instant access. <Button onClick={connectUsb} disabled={!!busy} className="font-medium text-blue-600 underline underline-offset-2 data-disabled:opacity-50">Connect over USB</Button></p>
      )}
      {!device.online && !link && <p className="rounded-xl bg-amber-50 p-3 text-xs text-amber-700 dark:bg-amber-950 dark:text-amber-300">The board is offline. Operations wait for it to reconnect.</p>}
      {supportsMount && !mounted && !link && <p className="rounded-xl bg-blue-50 p-3 text-xs text-blue-700 dark:bg-blue-950 dark:text-blue-300">Insert the microSD card, then mount it before opening files.</p>}
      <div className="flex flex-wrap items-center gap-2">
        <nav aria-label="Folder" className="flex min-w-0 flex-1 flex-wrap items-center gap-1 text-sm">
          {["/", ...path.split("/").filter(Boolean)].map((segment, i, all) => {
            const target = i === 0 ? "/" : "/" + all.slice(1, i + 1).join("/");
            return (
              <span key={target} className="flex items-center gap-1">
                {i > 0 && <span className="text-zinc-400">/</span>}
                <Button disabled={!!busy} onClick={() => open(target)} className="rounded px-1 font-medium text-blue-600 data-disabled:text-zinc-400">{i === 0 ? "Card" : segment}</Button>
              </span>
            );
          })}
        </nav>
        {supportsMount && !mounted && !link && (
          <Button
            disabled={!!busy || (!device.online && !link)}
            onClick={() => void perform("Mounting card…", async () => {
              const result = await change("sd_mount");
              setMounted(true);
              setNote(result);
              loaded.current = true;
              await loadFolder("/");
            })}
            className={secondaryButton + " h-8 px-3 text-xs"}
          >
            Mount card
          </Button>
        )}
        {supportsMount && mounted && (
          <Button
            disabled={!!busy}
            onClick={() => void perform("Unmounting card…", async () => {
              const result = await change("sd_unmount");
              setMounted(false);
              setEntries(null);
              setUsbCard(null);
              setNote(result);
              loaded.current = false;
            })}
            className={secondaryButton + " h-8 px-3 text-xs"}
          >
            Unmount
          </Button>
        )}
        <Button disabled={!!busy || path === "/"} onClick={() => open(parent(path))} className={secondaryButton + " h-8 px-3 text-xs"}>Up</Button>
        <Button disabled={!!busy || (!mounted && !link)} onClick={() => open(path)} className={secondaryButton + " h-8 px-3 text-xs"}>Refresh</Button>
        <Button disabled={!!busy || (!mounted && !link)} onClick={() => setNewFolder("")} className={secondaryButton + " h-8 px-3 text-xs"}>New folder</Button>
        <Button disabled={!!busy || (!mounted && !link)} onClick={() => upload.current?.click()} className={secondaryButton + " h-8 px-3 text-xs"}>Upload</Button>
        <input
          ref={upload}
          type="file"
          className="sr-only"
          aria-label="Upload a file to the SD card"
          onChange={(event) => {
            const file = event.currentTarget.files?.[0];
            event.currentTarget.value = "";
            if (!file) return;
            const usb = linkRef.current;
            if (file.size > (usb ? MAX_USB_TRANSFER : MAX_UPLOAD)) { setError(usb ? "Files uploaded over USB can be at most 512 MB." : "Files uploaded through the console can be at most 4 MB. Connect over USB for larger files."); return; }
            if (!validName(file.name)) { setError("This file name cannot be used on the card."); return; }
            void perform(`Uploading ${file.name}…`, async () => {
              if (usb) {
                await usb.upload(join(path, file.name), new Uint8Array(await file.arrayBuffer()), usbStage("sd_upload"));
              } else {
                const form = new FormData();
                form.set("file", file);
                form.set("path", join(path, file.name));
                await run(form);
              }
              const listing = await listFolder(path);
              setEntries(listing.entries);
            });
          }}
        />
      </div>

      {path.startsWith("/projects") && <p className="rounded-xl bg-amber-50 p-3 text-xs text-amber-700 dark:bg-amber-950 dark:text-amber-300">These files belong to installed display projects. Changing them can stop a project until it is reinstalled.</p>}

      {newFolder !== null && (
        <form className="flex gap-2" onSubmit={(e) => { e.preventDefault(); if (!validName(newFolder)) return; const name = newFolder.trim(); setNewFolder(null); void perform("Creating folder…", async () => { await change("sd_mkdir", join(path, name)); await loadFolder(path); }); }}>
          <input autoFocus value={newFolder} maxLength={64} placeholder="Folder name" onChange={(e) => setNewFolder(e.target.value)} className={inputClass + " h-9"} />
          <Button type="submit" disabled={!validName(newFolder)} className={secondaryButton + " h-9 px-3 text-xs"}>Create</Button>
          <Button onClick={() => setNewFolder(null)} className="h-9 px-2 text-xs text-zinc-500">Cancel</Button>
        </form>
      )}

      <ErrorText>{error}</ErrorText>
      {busy && <JobProgress label={busy} stage={stage} />}

      {entries === null ? (
        <p className="text-sm text-zinc-500">{busy ? "Loading…" : "Folder not loaded."}</p>
      ) : entries.length === 0 ? (
        <p className="rounded-xl border border-dashed border-zinc-300 p-6 text-center text-sm text-zinc-500 dark:border-zinc-700">This folder is empty.</p>
      ) : (
        <ul className="divide-y divide-zinc-200 rounded-xl border border-zinc-200 text-sm dark:divide-zinc-800 dark:border-zinc-800">
          {entries.map((entry) => (
            <li key={entry.name} className="flex flex-wrap items-center gap-2 px-3 py-2">
              {renaming?.entry === entry ? (
                <form className="flex min-w-0 flex-1 gap-2" onSubmit={(e) => { e.preventDefault(); const name = renaming.name.trim(); if (!validName(name) || name === entry.name) return; setRenaming(null); void perform("Renaming…", async () => { await change("sd_rename", join(path, entry.name), join(path, name)); await loadFolder(path); }); }}>
                  <input autoFocus value={renaming.name} maxLength={64} onChange={(e) => setRenaming({ entry, name: e.target.value })} className={inputClass + " h-8"} />
                  <Button type="submit" className={secondaryButton + " h-8 px-3 text-xs"}>Save</Button>
                  <Button onClick={() => setRenaming(null)} className="h-8 px-2 text-xs text-zinc-500">Cancel</Button>
                </form>
              ) : (
                <>
                  <button type="button" disabled={!entry.folder || !!busy} onClick={() => open(join(path, entry.name))} className="flex min-w-0 flex-1 items-center gap-2 text-left disabled:cursor-default">
                    <span aria-hidden className="text-base">{entry.folder ? "📁" : "📄"}</span>
                    <span className="min-w-0">
                      <span className={entry.folder ? "block truncate font-medium text-blue-600" : "block truncate"}>{entry.name}</span>
                      {!entry.folder && <span className="block text-xs text-zinc-500">{formatBytes(entry.size)}{entry.modified > 86_400_000 ? ` · ${new Date(entry.modified).toLocaleString()}` : ""}</span>}
                    </span>
                  </button>
                  {!entry.folder && (
                    <Button
                      disabled={!!busy || entry.size > (link ? MAX_USB_TRANSFER : MAX_UPLOAD)}
                      title={entry.size > (link ? MAX_USB_TRANSFER : MAX_UPLOAD) ? (link ? "Files over 512 MB cannot be downloaded over USB" : "Files over 4 MB need a USB connection to download") : undefined}
                      onClick={() => perform(`Downloading ${entry.name}…`, async () => {
                        const anchor = document.createElement("a");
                        anchor.download = entry.name;
                        const usb = linkRef.current;
                        if (usb) {
                          const blob = await usb.download(join(path, entry.name), usbStage("sd_download"));
                          anchor.href = URL.createObjectURL(blob);
                          anchor.click();
                          setTimeout(() => URL.revokeObjectURL(anchor.href), 10_000);
                        } else {
                          const job = await run({ type: "sd_download", path: join(path, entry.name) });
                          anchor.href = `/api/devices/${device.id}/files/${job.id}?content=1`;
                          anchor.click();
                        }
                      })}
                      className="h-8 px-2 text-xs text-blue-600 data-disabled:text-zinc-400"
                    >
                      Download
                    </Button>
                  )}
                  <Button disabled={!!busy} onClick={() => setRenaming({ entry, name: entry.name })} className="h-8 px-2 text-xs text-zinc-600 data-disabled:text-zinc-400 dark:text-zinc-300">Rename</Button>
                  <Button disabled={!!busy} onClick={() => setDeleting(entry)} className="h-8 px-2 text-xs text-red-600 data-disabled:text-zinc-400">Delete</Button>
                </>
              )}
            </li>
          ))}
        </ul>
      )}
      {note && entries && <p className="text-xs text-zinc-500">{note} · {link ? "USB transfers up to 512 MB" : "uploads and downloads up to 4 MB"}</p>}

      <ConfirmDialog
        open={!!deleting}
        onClose={() => setDeleting(null)}
        title={`Delete ${deleting?.name}?`}
        description={deleting?.folder ? "The folder and everything inside it are removed from the SD card. This cannot be undone." : "The file is removed from the SD card. This cannot be undone."}
        confirmLabel="Delete"
        onConfirm={() => {
          const entry = deleting!;
          setDeleting(null);
          void perform(`Deleting ${entry.name}…`, async () => {
            await change("sd_delete", join(path, entry.name));
            await loadFolder(path);
          });
        }}
      />
    </Sheet>
  );
}
