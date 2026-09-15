"use client";

import { Button } from "@headlessui/react";
import { useCallback, useEffect, useRef, useState } from "react";
import { ConfirmDialog, ErrorText, Sheet, inputClass, secondaryButton } from "@/components/ui";
import { api, compareVersions, deviceName } from "@/lib/device-client";
import { SD_MOUNT_FIRMWARE, type FileJob, type PublicDevice, type SdEntry } from "@/lib/device-types";
import { errorMessage } from "@/lib/esp";

const MAX_UPLOAD = 4 * 1024 * 1024;
const JOB_TIMEOUT_MS = 3.5 * 60 * 1000;

export function formatBytes(bytes: number) {
  if (bytes >= 1024 ** 3) return `${(bytes / 1024 ** 3).toFixed(1)} GB`;
  if (bytes >= 1024 ** 2) return `${(bytes / 1024 ** 2).toFixed(1)} MB`;
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${bytes} B`;
}

const join = (folder: string, name: string) => (folder === "/" ? `/${name}` : `${folder}/${name}`);
const parent = (path: string) => path.slice(0, path.lastIndexOf("/")) || "/";

/**
 * Starts a board operation and waits for its result. The board reaches the console on its own, so
 * every step is a queued command that the next board check-in picks up.
 */
export async function runFileJob(device: PublicDevice, body: object | FormData): Promise<FileJob> {
  const response = await fetch(`/api/devices/${device.id}/files`, {
    method: "POST",
    cache: "no-store",
    ...(body instanceof FormData ? { body } : { body: JSON.stringify(body), headers: { "Content-Type": "application/json" } }),
  });
  const started = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(started.error ?? `Request failed (${response.status})`);
  const deadline = Date.now() + JOB_TIMEOUT_MS;
  for (;;) {
    await new Promise((resolve) => setTimeout(resolve, 900));
    const { job } = await api<{ job: FileJob }>(`/api/devices/${device.id}/files/${started.job}`);
    if (job.status === "done") return job;
    if (job.status === "failed") throw new Error(job.result || "The board could not complete the operation.");
    if (Date.now() > deadline) throw new Error("The board did not answer in time. Check that it is online.");
  }
}

/** Browse, upload, download, rename and delete files on the board's SD card. */
export function SdFileManager({ device, onClose }: { device: PublicDevice; onClose: () => void }) {
  const [path, setPath] = useState("/");
  const [entries, setEntries] = useState<SdEntry[] | null>(null);
  const [note, setNote] = useState("");
  const [busy, setBusy] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [newFolder, setNewFolder] = useState<string | null>(null);
  const [renaming, setRenaming] = useState<{ entry: SdEntry; name: string } | null>(null);
  const [deleting, setDeleting] = useState<SdEntry | null>(null);
  const [mounted, setMounted] = useState(device.sdCard?.mounted ?? false);
  const upload = useRef<HTMLInputElement | null>(null);
  const loaded = useRef(false);
  const supportsMount = compareVersions(device.firmware, SD_MOUNT_FIRMWARE) >= 0;

  const perform = useCallback(async (label: string, action: () => Promise<void>) => {
    setBusy(label);
    setError(null);
    try {
      await action();
    } catch (err) {
      setError(errorMessage(err));
    } finally {
      setBusy(null);
    }
  }, []);

  const loadFolder = useCallback(async (folder: string) => {
    const job = await runFileJob(device, { type: "sd_list", path: folder });
    setPath(folder);
    setEntries(job.entries ?? []);
    setNote(job.result);
    setMounted(true);
  }, [device]);

  const open = useCallback((folder: string) => perform("Opening folder…", () => loadFolder(folder)), [loadFolder, perform]);

  useEffect(() => {
    if (loaded.current) return;
    // New firmware exposes an explicit mount action. Do not trap a card that has not been checked
    // yet behind a multi-minute automatic listing job; let the user mount it first.
    if (supportsMount && !mounted) return;
    loaded.current = true;
    const timer = window.setTimeout(() => void open("/"), 0);
    return () => window.clearTimeout(timer);
  }, [mounted, open, supportsMount]);

  const validName = (name: string) => name.trim() && !/[\\/\x00-\x1f]/.test(name) && name !== "." && name !== "..";
  const card = device.sdCard;

  return (
    <Sheet
      open
      wide
      onClose={onClose}
      title={`SD card · ${deviceName(device)}`}
      subtitle={mounted ? (card?.mounted ? `${formatBytes(card.free)} free of ${formatBytes(card.total)}` : "Card mounted; capacity will update on the next board check-in.") : "The board has not mounted a card."}
    >
      {!device.online && <p className="rounded-xl bg-amber-50 p-3 text-xs text-amber-700 dark:bg-amber-950 dark:text-amber-300">The board is offline. Operations wait for it to reconnect.</p>}
      {supportsMount && !mounted && <p className="rounded-xl bg-blue-50 p-3 text-xs text-blue-700 dark:bg-blue-950 dark:text-blue-300">Insert the microSD card, then mount it before opening files.</p>}
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
        {supportsMount && !mounted && (
          <Button
            disabled={!!busy || !device.online}
            onClick={() => void perform("Mounting card…", async () => {
              const job = await runFileJob(device, { type: "sd_mount" });
              setMounted(true);
              setNote(job.result);
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
              const job = await runFileJob(device, { type: "sd_unmount" });
              setMounted(false);
              setEntries(null);
              setNote(job.result);
              loaded.current = false;
            })}
            className={secondaryButton + " h-8 px-3 text-xs"}
          >
            Unmount
          </Button>
        )}
        <Button disabled={!!busy || path === "/"} onClick={() => open(parent(path))} className={secondaryButton + " h-8 px-3 text-xs"}>Up</Button>
        <Button disabled={!!busy || !mounted} onClick={() => open(path)} className={secondaryButton + " h-8 px-3 text-xs"}>Refresh</Button>
        <Button disabled={!!busy || !mounted} onClick={() => setNewFolder("")} className={secondaryButton + " h-8 px-3 text-xs"}>New folder</Button>
        <Button disabled={!!busy || !mounted} onClick={() => upload.current?.click()} className={secondaryButton + " h-8 px-3 text-xs"}>Upload</Button>
        <input
          ref={upload}
          type="file"
          className="sr-only"
          aria-label="Upload a file to the SD card"
          onChange={(event) => {
            const file = event.currentTarget.files?.[0];
            event.currentTarget.value = "";
            if (!file) return;
            if (file.size > MAX_UPLOAD) { setError("Files uploaded through the console can be at most 4 MB."); return; }
            if (!validName(file.name)) { setError("This file name cannot be used on the card."); return; }
            void perform(`Uploading ${file.name}…`, async () => {
              const form = new FormData();
              form.set("file", file);
              form.set("path", join(path, file.name));
              await runFileJob(device, form);
              const job = await runFileJob(device, { type: "sd_list", path });
              setEntries(job.entries ?? []);
            });
          }}
        />
      </div>

      {path.startsWith("/projects") && <p className="rounded-xl bg-amber-50 p-3 text-xs text-amber-700 dark:bg-amber-950 dark:text-amber-300">These files belong to installed display projects. Changing them can stop a project until it is reinstalled.</p>}

      {newFolder !== null && (
        <form className="flex gap-2" onSubmit={(e) => { e.preventDefault(); if (!validName(newFolder)) return; const name = newFolder.trim(); setNewFolder(null); void perform("Creating folder…", async () => { await runFileJob(device, { type: "sd_mkdir", path: join(path, name) }); await open(path); }); }}>
          <input autoFocus value={newFolder} maxLength={64} placeholder="Folder name" onChange={(e) => setNewFolder(e.target.value)} className={inputClass + " h-9"} />
          <Button type="submit" disabled={!validName(newFolder)} className={secondaryButton + " h-9 px-3 text-xs"}>Create</Button>
          <Button onClick={() => setNewFolder(null)} className="h-9 px-2 text-xs text-zinc-500">Cancel</Button>
        </form>
      )}

      <ErrorText>{error}</ErrorText>
      {busy && <p role="status" className="text-xs text-blue-600">{busy} Waiting for the board…</p>}

      {entries === null ? (
        <p className="text-sm text-zinc-500">{busy ? "Loading…" : "Folder not loaded."}</p>
      ) : entries.length === 0 ? (
        <p className="rounded-xl border border-dashed border-zinc-300 p-6 text-center text-sm text-zinc-500 dark:border-zinc-700">This folder is empty.</p>
      ) : (
        <ul className="divide-y divide-zinc-200 rounded-xl border border-zinc-200 text-sm dark:divide-zinc-800 dark:border-zinc-800">
          {entries.map((entry) => (
            <li key={entry.name} className="flex flex-wrap items-center gap-2 px-3 py-2">
              {renaming?.entry === entry ? (
                <form className="flex min-w-0 flex-1 gap-2" onSubmit={(e) => { e.preventDefault(); const name = renaming.name.trim(); if (!validName(name) || name === entry.name) return; setRenaming(null); void perform("Renaming…", async () => { await runFileJob(device, { type: "sd_rename", path: join(path, entry.name), to: join(path, name) }); await open(path); }); }}>
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
                      disabled={!!busy || entry.size > MAX_UPLOAD}
                      title={entry.size > MAX_UPLOAD ? "Files over 4 MB cannot be downloaded through the console" : undefined}
                      onClick={() => perform(`Downloading ${entry.name}…`, async () => {
                        const job = await runFileJob(device, { type: "sd_download", path: join(path, entry.name) });
                        const link = document.createElement("a");
                        link.href = `/api/devices/${device.id}/files/${job.id}?content=1`;
                        link.download = entry.name;
                        link.click();
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
      {note && entries && <p className="text-xs text-zinc-500">{note} · uploads and downloads up to 4 MB</p>}

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
            await runFileJob(device, { type: "sd_delete", path: join(path, entry.name) });
            await open(path);
          });
        }}
      />
    </Sheet>
  );
}
