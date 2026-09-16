"use client";

import { Button } from "@headlessui/react";
import { useSession } from "next-auth/react";
import { useEffect, useRef, useState } from "react";
import { AuthCard } from "@/components/AuthCard";
import { ProjectBuilderGuide } from "@/components/ProjectBuilderGuide";
import { Sheet, ToastBanner, accentButton, cardClass, inputClass, secondaryButton, useToast } from "@/components/ui";
import { api, deviceName, isOtaActive } from "@/lib/device-client";
import type { PublicDevice } from "@/lib/device-types";
import { errorMessage } from "@/lib/esp";

const DRAFT_KEY = "esp32-console-project-builder-v1";
const BUILDER_BOARD = "esp32-s3-lcd-0.85";
const STARTER = `#include "project_builder.h"

void setup(ProjectBoard *board) {
  board->led_fill(0, 20, 80);
  board->led_show();
}

void loop(ProjectBoard *board) {
  project_clear(board, project_rgb(8, 12, 24));
  project_text(board, 28, "Hello, ESP32!", board->text, 1);

  if (project_button_pressed(board, PROJECT_BUTTON_PLUS)) {
    project_text(board, 48, "PLUS pressed", board->accent, 1);
  }

  int x = (int)(board->frame_ms / 20u) % board->width;
  board->circle(board->canvas, x, 70, 5, board->accent);
}

PROJECT_SKETCH()
`;

type Draft = { id: string; name: string; version: string; description: string; source: string };
const INITIAL: Draft = { id: "my-project", name: "My project", version: "1.0.0", description: "Built in ESP32 Console", source: STARTER };

export function ProjectBuilder({ active }: { active: boolean }) {
  const { data: session, status } = useSession();
  const user = session?.user;
  const [draft, setDraft] = useState<Draft>(INITIAL);
  const [loaded, setLoaded] = useState(false);
  const [draftStatus, setDraftStatus] = useState<"saving" | "saved" | "error">("saved");
  const revision = useRef(0);
  const [devices, setDevices] = useState<PublicDevice[] | null>(null);
  const [deviceId, setDeviceId] = useState("");
  const [binary, setBinary] = useState<Blob | null>(null);
  const [building, setBuilding] = useState(false);
  const [installing, setInstalling] = useState(false);
  const [guideOpen, setGuideOpen] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [toast, setToast] = useToast();

  useEffect(() => {
    const frame = requestAnimationFrame(() => {
      try {
        const value = localStorage.getItem(DRAFT_KEY);
        if (value) {
          const stored = JSON.parse(value) as Partial<Draft>;
          if (stored && typeof stored === "object") {
            setDraft({
              id: typeof stored.id === "string" ? stored.id : INITIAL.id,
              name: typeof stored.name === "string" ? stored.name : INITIAL.name,
              version: typeof stored.version === "string" ? stored.version : INITIAL.version,
              description: typeof stored.description === "string" ? stored.description : INITIAL.description,
              source: typeof stored.source === "string" ? stored.source : INITIAL.source,
            });
          }
        }
      } catch { /* Keep the starter sketch when browser storage is unavailable. */ }
      setLoaded(true);
    });
    return () => cancelAnimationFrame(frame);
  }, []);

  useEffect(() => {
    if (!loaded) return;
    const timer = setTimeout(() => {
      try {
        localStorage.setItem(DRAFT_KEY, JSON.stringify(draft));
        setDraftStatus("saved");
      } catch { setDraftStatus("error"); }
    }, 600);
    return () => clearTimeout(timer);
  }, [draft, loaded]);

  useEffect(() => {
    if (!active || !user) return;
    api<{ devices: PublicDevice[] }>("/api/devices")
      .then(({ devices }) => {
        const supported = devices.filter((entry) => entry.board === BUILDER_BOARD);
        setDevices(supported);
        setDeviceId((current) => supported.some((entry) => entry.id === current) ? current : supported[0]?.id || "");
      })
      .catch((err) => setError(errorMessage(err)));
  }, [active, user]);

  const change = <K extends keyof Draft>(key: K, value: Draft[K]) => {
    revision.current += 1;
    setDraft((current) => ({ ...current, [key]: value }));
    setBinary(null); setDraftStatus("saving"); setError(null);
  };
  const saveDraft = () => {
    try {
      localStorage.setItem(DRAFT_KEY, JSON.stringify(draft));
      setDraftStatus("saved"); setToast("Draft saved in this browser.");
    } catch { setDraftStatus("error"); setError("This browser could not save the draft. Download the .ino file instead."); }
  };
  const newSketch = () => {
    if (!window.confirm("Start a new sketch? This will replace the current draft in this browser.")) return;
    revision.current += 1;
    try { localStorage.removeItem(DRAFT_KEY); } catch { /* The save status will report unavailable storage. */ }
    setDraft(INITIAL);
    setBinary(null);
    setDraftStatus("saving");
    setError(null);
  };
  const build = async () => {
    const buildRevision = revision.current;
    setBuilding(true); setError(null); setBinary(null);
    try {
      const response = await fetch("/api/project-builder", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(draft),
      });
      if (!response.ok) {
        const body = await response.json().catch(() => ({}));
        throw new Error(body.error || `Build failed (${response.status}).`);
      }
      const elf = await response.blob();
      if (buildRevision === revision.current) {
        setBinary(elf); setToast(`Build succeeded · ${elf.size.toLocaleString()} bytes`);
      }
    } catch (err) { if (buildRevision === revision.current) setError(errorMessage(err)); }
    finally {
      if (buildRevision !== revision.current) setError("The sketch changed while compiling. Compile again to install or download the current version.");
      setBuilding(false);
    }
  };
  const install = async () => {
    if (!binary || !deviceId) return;
    setInstalling(true); setError(null);
    try {
      const form = new FormData();
      form.set("file", new File([binary], `${draft.id}-${draft.version}.elf`, { type: "application/x-elf" }));
      const response = await fetch(`/api/devices/${deviceId}/projects`, { method: "POST", body: form });
      const body = await response.json().catch(() => ({}));
      if (!response.ok) throw new Error(body.error || "Installation request failed.");
      setToast("Installation queued. Progress is available in Projects.");
    } catch (err) { setError(errorMessage(err)); }
    finally { setInstalling(false); }
  };

  if (status === "loading" || !loaded) return <p className="py-8 text-center text-sm text-zinc-500">Loading…</p>;
  if (!user) return <AuthCard />;
  const device = devices?.find((entry) => entry.id === deviceId);
  const cannotInstall = !binary || !device || installing || !device.projectSupported || device.projectApi < 4 || isOtaActive(device.ota);

  return (
    <div className="space-y-4">
      <div>
        <h2 className="text-sm font-semibold">Project Builder</h2>
        <p className="mt-1 text-xs text-zinc-500">Write an Arduino-style sketch, compile a standalone Xtensa ELF, then download it or install it on a board.</p>
      </div>
      {error && <pre role="alert" className="max-h-64 overflow-auto whitespace-pre-wrap rounded-xl bg-red-50 p-3 font-mono text-xs leading-5 text-red-700 dark:bg-red-950/50 dark:text-red-300">{error}</pre>}
      <div className="grid items-start gap-4 xl:grid-cols-[minmax(0,1fr)_18rem]">
        <section className={cardClass + " min-w-0 overflow-hidden"}>
          <div className="grid gap-3 border-b border-zinc-200 p-4 sm:grid-cols-2 dark:border-zinc-800">
            <Field label="Project name"><input className={inputClass} value={draft.name} maxLength={80} onChange={(event) => change("name", event.target.value)} /></Field>
            <Field label="Project ID"><input className={inputClass + " font-mono"} value={draft.id} maxLength={48} spellCheck={false} onChange={(event) => change("id", event.target.value.toLowerCase().replace(/[^a-z0-9-]/g, ""))} /></Field>
            <Field label="Version"><input className={inputClass + " font-mono"} value={draft.version} inputMode="numeric" onChange={(event) => change("version", event.target.value)} /></Field>
            <Field label="Description"><input className={inputClass} value={draft.description} maxLength={300} onChange={(event) => change("description", event.target.value)} /></Field>
          </div>
          <div className="flex items-center justify-between gap-3 border-b border-zinc-200 bg-zinc-50 px-4 py-2 dark:border-zinc-800 dark:bg-zinc-950/50">
            <span className="font-mono text-xs text-zinc-500">{draft.id || "project"}.ino</span>
            <span className="text-xs text-zinc-500">C-compatible sketch · ABI 4</span>
          </div>
          <textarea
            aria-label="Project source code"
            className="min-h-[32rem] w-full resize-y bg-zinc-950 p-4 font-mono text-[13px] leading-6 text-zinc-100 outline-none focus:ring-2 focus:ring-inset focus:ring-blue-500"
            value={draft.source}
            spellCheck={false}
            onChange={(event) => change("source", event.target.value)}
            onKeyDown={(event) => {
              if (event.key !== "Tab") return;
              event.preventDefault();
              const input = event.currentTarget, start = input.selectionStart, end = input.selectionEnd;
              change("source", draft.source.slice(0, start) + "  " + draft.source.slice(end));
              requestAnimationFrame(() => { input.selectionStart = input.selectionEnd = start + 2; });
            }}
          />
          <div className="flex flex-wrap items-center gap-2 border-t border-zinc-200 p-4 dark:border-zinc-800">
            <Button onClick={build} disabled={building} className={accentButton + " h-10 px-4"}>{building ? "Compiling…" : "Compile ELF"}</Button>
            <Button onClick={saveDraft} className={secondaryButton + " h-10 px-4"}>Save now</Button>
            <span role="status" className={`text-xs ${draftStatus === "error" ? "text-red-600 dark:text-red-400" : "text-zinc-500"}`}>
              {draftStatus === "error" ? "Could not save in this browser. Download the .ino file." : draftStatus === "saving" ? "Saving draft…" : "Saved in this browser"}
            </span>
            <label className={secondaryButton + " flex h-10 cursor-pointer items-center px-4"}>
              Open .ino
              <input
                type="file"
                accept=".ino,.c,text/plain"
                className="sr-only"
                onChange={async (event) => {
                  const file = event.target.files?.[0]; event.target.value = "";
                  if (!file) return;
                  if (!file.size || file.size > 48 * 1024) { setError("Sketch files must be between 1 byte and 48 KB."); return; }
                  try { change("source", await file.text()); } catch { setError("The sketch file could not be read."); }
                }}
              />
            </label>
            <Button onClick={() => download(new Blob([draft.source], { type: "text/plain" }), `${draft.id || "project"}.ino`)} className={secondaryButton + " h-10 px-4"}>Download .ino</Button>
            {binary && <Button onClick={() => download(binary, `${draft.id}-${draft.version}.elf`)} className={secondaryButton + " h-10 px-4"}>Download ELF · {binary.size.toLocaleString()} B</Button>}
            <Button onClick={newSketch} className="ml-auto h-10 px-2 text-sm text-zinc-500 underline">New sketch</Button>
          </div>
        </section>

        <aside className="space-y-4 xl:sticky xl:top-4">
          <section className={cardClass + " space-y-3 p-4"}>
            <h3 className="text-sm font-semibold">Install on board</h3>
            {devices === null ? <p className="text-xs text-zinc-500">Loading boards…</p> : devices.length === 0 ? <p className="text-xs text-zinc-500">Add an ESP32-S3-LCD-0.85 board in Devices to install Builder projects. Other boards are not supported yet.</p> : (
              <select className={inputClass} value={deviceId} onChange={(event) => setDeviceId(event.target.value)}>
                {devices.map((entry) => <option key={entry.id} value={entry.id}>{deviceName(entry)} · {entry.online ? "Online" : "Offline"}</option>)}
              </select>
            )}
            {device && device.projectApi < 4 && <p className="text-xs text-amber-600">Update the board to firmware v1.1.9 or newer before installing Builder projects.</p>}
            <Button disabled={cannotInstall} onClick={install} className={accentButton + " h-10 w-full px-4"}>{installing ? "Sending…" : binary ? "Install compiled ELF" : "Compile before install"}</Button>
            <p className="text-xs text-zinc-500">Installing replaces the active display project. The base firmware remains intact and safe mode can recover a faulty project.</p>
          </section>
          <section className={cardClass + " space-y-3 p-4 text-xs"}>
            <div className="flex items-center justify-between gap-2"><h3 className="text-sm font-semibold">Board API</h3><a href="/api/project-builder" className="text-blue-600 underline">Download header</a></div>
            <Button onClick={() => setGuideOpen(true)} className="block w-full rounded-lg bg-blue-50 p-2 text-left font-medium text-blue-700 data-hover:bg-blue-100 dark:bg-blue-950/50 dark:text-blue-300 dark:data-hover:bg-blue-950">Open coding guide and examples</Button>
            <p className="text-zinc-500">One include exposes the stable, bounded ABI 4 board capabilities.</p>
            <Api label="Display" value="128 × 128 · RGB565" />
            <Api label="Drawing" value="text, line, ring, circle, rectangles" />
            <Api label="Runtime" value="clock, frame time, theme colors" />
            <Api label="Storage" value="sandboxed SD read/write/list" />
            <Api label="Input/output" value="buttons · battery · 8 LEDs" />
            <Api label="Audio" value="stereo mic and speaker PCM" />
            <Api label="Network" value="Wi-Fi scan · HTTP(S) GET · BLE scan" />
            <p className="rounded-lg bg-zinc-100 p-2 text-zinc-600 dark:bg-zinc-800 dark:text-zinc-300">Slow operations run in the background; SD, audio and response sizes are bounded so the Console remains responsive.</p>
          </section>
        </aside>
      </div>
      <Sheet open={guideOpen} onClose={() => setGuideOpen(false)} title="Project Builder coding guide" subtitle="ESP32-S3-LCD-0.85 · ABI 4" wide>
        <ProjectBuilderGuide />
      </Sheet>
      <ToastBanner toast={toast} />
    </div>
  );
}

function Field({ label, children }: { label: string; children: React.ReactNode }) {
  return <label className="space-y-1.5"><span className="block text-xs font-medium text-zinc-500">{label}</span>{children}</label>;
}

function Api({ label, value }: { label: string; value: string }) {
  return <p className="flex justify-between gap-3"><span className="font-medium">{label}</span><span className="text-right text-zinc-500">{value}</span></p>;
}

function download(blob: Blob, name: string) {
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement("a");
  anchor.href = url; anchor.download = name; anchor.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
