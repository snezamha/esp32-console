"use client";

import { Button, Tab, TabGroup, TabList, TabPanel, TabPanels } from "@headlessui/react";
import { useSession } from "next-auth/react";
import { useEffect, useRef, useState, type RefObject } from "react";
import { ProjectSettingsForm } from "@/components/ProjectSettingsForm";
import { AuthCard } from "@/components/AuthCard";
import { Select } from "@/components/Select";
import { ConfirmDialog, ErrorText, Skeleton, ToastBanner, accentButton, cardClass, useToast } from "@/components/ui";
import { api, deviceName, isOtaActive } from "@/lib/device-client";
import { inspectProjectUpload } from "@/lib/project-file";
import { PROJECT_DEFINITIONS, type ProjectConfig, type ProjectDefinition } from "@/lib/project-config";
import { projectPending } from "@/lib/project-transfers";
import type { DeviceCommand, PublicDevice } from "@/lib/device-types";
import { errorMessage } from "@/lib/esp";

const PROJECTS = [...PROJECT_DEFINITIONS, { id: "none", name: "Default display", description: "Unload the project and return to the base display." }];
type Project = (typeof PROJECTS)[number];

export function Projects({ active }: { active: boolean }) {
  const { data: session, status } = useSession();
  const user = session?.user;
  const [devices, setDevices] = useState<PublicDevice[] | null>(null);
  const [selected, setSelected] = useState("");
  const updates = useRef(0);
  const [error, setError] = useState<string | null>(null);
  const [connection, setConnection] = useState<"connecting" | "live" | "retrying">("connecting");
  const installing = devices?.some((device) => {
    return device.commands.some((command) => command.type.startsWith("project_") && projectPending(command));
  }) ?? false;

  // Fetch a snapshot when entering the tab; an idle Projects tab needs no live connection.
  useEffect(() => {
    if (!active || !user) return;
    let cancelled = false;
    const revision = updates.current;
    api<{ devices: PublicDevice[] }>("/api/devices")
      .then(({ devices }) => {
        if (!cancelled && revision === updates.current) { setDevices(devices); setError(null); }
      })
      .catch((err) => { if (!cancelled && revision === updates.current) setError(errorMessage(err)); });
    return () => { cancelled = true; };
  }, [active, user]);

  // Follow actual installations until confirmation or failure, then close the connection.
  useEffect(() => {
    if (!active || !user || !installing) return;
    let source: EventSource | null = null;
    let retryTimer: ReturnType<typeof setTimeout> | null = null;
    let stopped = false;
    let failures = 0;
    const open = () => {
      if (stopped) return;
      source?.close();
      source = new EventSource("/api/devices/stream");
      source.addEventListener("devices", (event) => {
        updates.current += 1;
        setDevices(JSON.parse((event as MessageEvent).data));
        failures = 0;
        setConnection("live");
      });
      source.onerror = () => {
        source?.close();
        failures += 1;
        if (failures >= 3) setConnection("retrying");
        retryTimer = setTimeout(() => { if (document.visibilityState === "visible") open(); }, Math.min(4000, 500 * failures));
      };
    };
    const onVisibility = () => {
      if (document.visibilityState === "visible") { setConnection("connecting"); open(); }
      else source?.close();
    };
    onVisibility();
    document.addEventListener("visibilitychange", onVisibility);
    return () => {
      stopped = true;
      if (retryTimer) clearTimeout(retryTimer);
      source?.close();
      document.removeEventListener("visibilitychange", onVisibility);
    };
  }, [active, user, installing]);
  if (status === "loading") return <p className="py-8 text-center text-sm text-zinc-500">Loading…</p>;
  if (!user) return <AuthCard />;
  const device = devices?.find((d) => d.id === selected) ?? devices?.[0];
  return (
    <div className="space-y-4">
      <div>
        <h2 className="text-sm font-semibold">Projects</h2>
        <p className="mt-1 text-xs text-zinc-500">One active project per board. Each project is a separate file uploaded to the board’s flash. It runs below the status bar.</p>
      </div>
      <ErrorText>{error}</ErrorText>
      {devices === null && (
        <div className="space-y-3" aria-hidden>
          <Skeleton className="h-11 w-full" />
          <Skeleton className="h-64 w-full" />
        </div>
      )}
      {devices?.length === 0 && <p className="rounded-xl border border-dashed border-zinc-300 p-6 text-sm text-zinc-500">Add a board in Devices to install a project.</p>}
      {device && <>
        <Select label="Board" value={device} options={devices ?? []} onChange={(d) => setSelected(d.id)} getKey={(d) => d.id} renderValue={(d) => `${deviceName(d)} · ${d.online ? "Online" : "Offline"}`} />
        {installing && connection === "retrying" && <p className="text-xs text-amber-600">Reconnecting to live installation status…</p>}
        <ProjectPicker key={device.id} device={device} onUpdated={(updated) => {
          updates.current += 1;
          setDevices((current) => current?.map((d) => d.id === updated.id ? updated : d) ?? [updated]);
        }} />
      </>}
    </div>
  );
}

function ProjectPicker({ device, onUpdated }: { device: PublicDevice; onUpdated: (device: PublicDevice) => void }) {
  const [busy, setBusy] = useState(false);
  const [tab, setTab] = useState(Math.max(0, PROJECTS.findIndex((p) => p.id === device.activeProject)));
  const [saved, setSaved] = useState<{ project: string; message: string } | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [toast, setToast] = useToast();
  const [confirmClear, setConfirmClear] = useState(false);
  const [configs, setConfigs] = useState(device.projectSettings);
  const [file, setFile] = useState<File | null>(null);
  const [fileName, setFileName] = useState("");
  const [upload, setUpload] = useState<number | null>(null);
  const xhr = useRef<XMLHttpRequest | null>(null);
  const installation = device.commands.findLast((command) => command.type === "project_install" && (tab < PROJECTS.length ? new URLSearchParams(command.arg).get("id") === PROJECTS[tab].id : new URLSearchParams(command.arg).get("path")?.startsWith("/api/devices/")));
  const stopping = device.commands.some((command) => command.type === "project_stop" && projectPending(command));
  const loading = device.commands.some((command) => command.type === "project_install" && projectPending(command));
  const disabled = busy || loading || stopping || !device.projectSupported || isOtaActive(device.ota);
  const modern = device.firmware.localeCompare("1.0.9", undefined, { numeric: true }) >= 0 && device.projectApi >= 2;
  const run = async (action: () => Promise<void>) => {
    setBusy(true); setError(null); setSaved(null);
    try { await action(); } catch (err) { setError(errorMessage(err)); } finally { setBusy(false); }
  };
  const command = async (body: object) => {
    const result = await api<{ device: PublicDevice }>(`/api/devices/${device.id}/commands`, "POST", body);
    onUpdated(result.device);
  };
  const save = async (project: string) => {
    const definition = PROJECT_DEFINITIONS.find((entry) => entry.id === project);
    if (!definition) return;
    const config = configs[project];
    const boardSettings = Object.fromEntries(definition.settings.flatMap((setting) => setting.type === "timezone" ? [["tz", config[setting.key]]] : []));
    const result = await api<{ device: PublicDevice }>(`/api/devices/${device.id}`, "PATCH", { project, projectSettings: config, ...(Object.keys(boardSettings).length ? { settings: boardSettings } : {}) });
    onUpdated(result.device); setSaved({ project, message: "Settings saved. Applied when the board connects." });
  };
  const install = (project: string) => run(async () => {
    if (project !== "none") await save(project);
    await command({ type: "project_install", project });
    setToast(`Installing ${PROJECTS.find((p) => p.id === project)?.name ?? project}…`);
  });
  const reinstall = (historyId: string) => run(async () => {
    await command({ type: "project_install", retry: historyId });
    setToast("Reinstalling previous version…");
  });
  const uploadFile = () => run(async () => {
    if (!file) throw new Error("Choose a project file.");
    await new Promise<void>((resolve, reject) => {
      const request = new XMLHttpRequest(); xhr.current = request;
      request.open("POST", `/api/devices/${device.id}/projects`); request.timeout = 30_000;
      request.upload.onprogress = (event) => { if (event.lengthComputable) setUpload(Math.round(event.loaded * 100 / event.total)); };
      request.onload = () => {
        try { const result = JSON.parse(request.responseText); if (request.status >= 400) reject(new Error(result.error || "Upload failed.")); else { onUpdated(result.device); resolve(); } } catch { reject(new Error("Upload failed.")); }
      };
      request.onerror = () => reject(new Error("Upload connection failed. Retry."));
      request.ontimeout = () => reject(new Error("Upload timed out. Retry."));
      request.onabort = () => reject(new Error("File upload cancelled."));
      const form = new FormData(); form.set("file", file); setUpload(0); request.send(form);
    }).finally(() => { setUpload(null); xhr.current = null; });
  });
  const transfer = installation?.transfer;
  const logs = transfer?.logs ?? (installation ? [{ seq: 0, at: installation.createdAt, level: "info", message: "Installation requested." }, { seq: 1, at: installation.updatedAt, level: installation.status === "failed" ? "error" : "info", message: installation.result || (installation.status === "queued" ? "Waiting for board. Request expires after 10 minutes." : "Request delivered. Update to firmware 1.0.8 for board logs and the corrected native runtime.") }] : []);
  const logEnd = useRef<HTMLDivElement | null>(null);
  useEffect(() => { logEnd.current?.scrollIntoView({ block: "nearest" }); }, [logs.length]);
  return <div className="space-y-4">
    {!modern && <p className="rounded-xl bg-amber-50 p-3 text-xs text-amber-700 dark:bg-amber-950 dark:text-amber-300">Install base firmware v1.0.9 in Devices → Details → Firmware before loading a project. It validates project identity and SHA-256 and adds automatic safe mode.</p>}
    {device.projectSafeMode && <p className="rounded-xl bg-amber-50 p-3 text-xs text-amber-700 dark:bg-amber-950 dark:text-amber-300">Project safe mode is active after repeated crashes or a Vol+ boot. The default display is running; reinstall a trusted project when ready.</p>}
    <section className={cardClass + " flex items-center justify-between gap-3 p-4"}><div><p className="text-xs text-zinc-500">Active on display</p><p className="mt-1 text-sm font-semibold">{PROJECTS.find((p) => p.id === device.activeProject)?.name ?? device.activeProject}</p></div><span className={device.online ? "text-xs text-emerald-600" : "text-xs text-zinc-500"}>{device.online ? "Board online" : "Board offline"}</span></section>
    <ErrorText>{error}</ErrorText>
    {loading && !projectPending(installation) && <Button className="text-xs text-blue-600 underline" onClick={() => { const pending = device.commands.findLast((c) => c.type === "project_install" && projectPending(c)); const id = new URLSearchParams(pending?.arg).get("id"); const index = PROJECTS.findIndex((p) => p.id === id); setTab(index < 0 ? PROJECTS.length : index); }}>Another project is installing · open its log and controls</Button>}
    {installation && (
      <InstallationCard
        device={device}
        installation={installation}
        logs={logs}
        logEnd={logEnd}
        loading={loading}
        stopping={stopping}
        disabled={disabled}
        busy={busy}
        onStop={() => run(() => command({ type: "project_stop", command: installation.id }))}
        onRetry={() => {
          const id = new URLSearchParams(installation.arg).get("id") ?? "none";
          if (!new URLSearchParams(installation.arg).get("path")?.startsWith("/api/devices/") && PROJECTS.some((p) => p.id === id)) install(id);
          else run(async () => { await command({ type: "project_install", retry: installation.id }); });
        }}
        onRefresh={() => run(async () => { const result = await api<{ devices: PublicDevice[] }>("/api/devices"); const current = result.devices.find((d) => d.id === device.id); if (current) onUpdated(current); })}
      />
    )}
    <TabGroup selectedIndex={tab} onChange={(index) => { setTab(index); setSaved(null); setError(null); }} className="grid items-start gap-4 lg:grid-cols-[15rem_minmax(0,1fr)]">
      <TabList className="grid grid-cols-2 gap-2 rounded-xl bg-zinc-100 p-2 sm:grid-cols-4 lg:sticky lg:top-4 lg:grid-cols-1 dark:bg-zinc-800">{[...PROJECTS.map((p) => p.name), "Upload project"].map((name) => <Tab key={name} className="rounded-lg px-4 py-3 text-left text-xs font-medium outline-none data-selected:bg-white data-selected:shadow-sm data-focus:ring-2 data-focus:ring-blue-500 dark:data-selected:bg-zinc-900">{name}</Tab>)}</TabList>
      <TabPanels>
        {PROJECTS.map((project) => (
          <ProjectTabPanel
            key={project.id}
            project={project}
            device={device}
            busy={busy}
            loading={loading}
            stopping={stopping}
            disabled={disabled}
            modern={modern}
            saved={saved}
            config={project.id === "none" ? {} : configs[project.id]}
            onConfig={(value) => { setConfigs((current) => ({ ...current, [project.id]: value })); setSaved(null); }}
            onSave={() => run(() => save(project.id))}
            onInstall={() => install(project.id)}
          />
        ))}
        <TabPanel>
          <UploadPanel
            busy={busy}
            disabled={disabled}
            modern={modern}
            file={file}
            fileName={fileName}
            upload={upload}
            xhr={xhr}
            onFile={async (selected) => {
              setFile(null); setFileName(""); setError(null);
              if (!selected) return;
              try {
                const meta = await inspectProjectUpload(selected);
                setFile(selected); setFileName(`${meta.name} · v${meta.version} · ${selected.size.toLocaleString()} bytes`);
              } catch (err) { setError(errorMessage(err)); }
            }}
            onUpload={uploadFile}
          />
        </TabPanel>
      </TabPanels>
    </TabGroup>
    <HistoryPanel
      device={device}
      installation={installation}
      disabled={disabled}
      busy={busy}
      confirmClear={confirmClear}
      onClear={() => setConfirmClear(true)}
      onConfirmClear={() => { setConfirmClear(false); run(async () => { const result = await api<{ device: PublicDevice }>(`/api/devices/${device.id}/projects`, "DELETE"); onUpdated(result.device); setToast("History and logs cleared."); }); }}
      onCancelClear={() => setConfirmClear(false)}
      onReinstall={reinstall}
    />
    <ToastBanner toast={toast} />
  </div>;
}

function InstallationCard({
  device,
  installation,
  logs,
  logEnd,
  loading,
  stopping,
  disabled,
  busy,
  onStop,
  onRetry,
  onRefresh,
}: {
  device: PublicDevice;
  installation: DeviceCommand;
  logs: { seq: number; at: number; level: string; message: string }[];
  logEnd: RefObject<HTMLDivElement | null>;
  loading: boolean;
  stopping: boolean;
  disabled: boolean;
  busy: boolean;
  onStop: () => void;
  onRetry: () => void;
  onRefresh: () => void;
}) {
  const transfer = installation.transfer;
  return (
    <section className={cardClass + " space-y-3 p-4"}>
      <div className="flex items-center justify-between gap-3"><h3 className="text-sm font-semibold">Installation · {transfer?.name ?? new URLSearchParams(installation.arg).get("id")}</h3><span className="text-xs capitalize">{transfer?.phase ?? installation.status}</span></div>
      <progress aria-label="Board download progress" value={transfer?.progress ?? 0} max={100} className="h-2 w-full accent-blue-600" />
      <p className="text-xs text-zinc-500">{transfer ? `${transfer.progress}% · ${transfer.bytes.toLocaleString()} / ${transfer.total.toLocaleString()} bytes${transfer.version ? ` · v${transfer.version}` : ""}` : "Waiting for installation report"}</p>
      {loading && device.firmware.localeCompare("1.0.5", undefined, { numeric: true }) < 0 && <p className="text-xs text-amber-600">Stopping on this older firmware restarts the board to interrupt its download.</p>}
      {loading && !device.online && <p role="status" className="text-xs text-amber-600">Board offline. Reconnect its power and Wi-Fi. You can cancel this request; waiting is limited.</p>}
      <div role="log" aria-label="Installation logs" aria-live="polite" className="max-h-64 overflow-auto rounded-lg bg-zinc-950 p-3 font-mono text-xs text-zinc-300">{logs.map((log) => <p key={log.seq} className={log.level === "error" ? "text-red-400" : ""}>{new Date(log.at).toLocaleTimeString()} · {log.message}</p>)}<div ref={logEnd} /></div>
      <ErrorText>{installation.status === "failed" && transfer?.phase !== "cancelled" ? installation.result : null}</ErrorText>
      {installation.status === "failed" && installation.result === "No memory for project download" && device.firmware.localeCompare("1.1.24", undefined, { numeric: true }) < 0 && (
        <p className="rounded-lg bg-amber-50 px-3 py-2 text-xs text-amber-800 dark:bg-amber-950/40 dark:text-amber-200">The active radio stream is using the memory needed to start this download. Update the base firmware in Devices → Details → Firmware, then retry.</p>
      )}
      <div className="flex gap-2">
        {projectPending(installation) && <Button disabled={busy} onClick={onStop} className={accentButton + " h-9 px-4"}>Stop installation</Button>}
        {!projectPending(installation) && installation.status === "failed" && <Button disabled={disabled} onClick={onRetry} className={accentButton + " h-9 px-4"}>Retry</Button>}
        <Button disabled={busy} onClick={onRefresh} className="h-9 px-3 text-xs text-zinc-500">Refresh status</Button>
      </div>
      {stopping && <p role="status" className="text-xs text-amber-600">Stop requested; waiting for board confirmation before another installation.</p>}
    </section>
  );
}

function ProjectTabPanel({
  project,
  device,
  busy,
  loading,
  stopping,
  disabled,
  modern,
  saved,
  config,
  onConfig,
  onSave,
  onInstall,
}: {
  project: Project;
  device: PublicDevice;
  busy: boolean;
  loading: boolean;
  stopping: boolean;
  disabled: boolean;
  modern: boolean;
  saved: { project: string; message: string } | null;
  config: ProjectConfig;
  onConfig: (value: ProjectConfig) => void;
  onSave: () => void;
  onInstall: () => void;
}) {
  const storage = (project as Partial<ProjectDefinition>).storage;
  const requiredFirmware = (project as Partial<ProjectDefinition>).minimumFirmware;
  const firmwareTooOld = !!requiredFirmware && device.firmware.localeCompare(requiredFirmware, undefined, { numeric: true }) < 0;
  return (
    <TabPanel className={cardClass + " space-y-4 p-4"}>
      <div className="flex items-center justify-between"><h3 className="text-sm font-semibold">{project.name}</h3>{device.activeProject === project.id && <span className="text-xs text-emerald-600">Active on board</span>}</div>
      <p className="text-xs text-zinc-500">{project.description}</p>
      {"version" in project && <><div className="flex flex-wrap gap-2 text-[11px] text-zinc-500"><span>Catalog v{project.version}</span>{device.activeProject === project.id && device.activeProjectVersion && <span>Installed v{device.activeProjectVersion}</span>}<span>{(project.size / 1024).toFixed(1)} KB</span>{storage && <span>SD card · {megabytes(storage.bytes)} · {storage.files} files</span>}</div><details className="text-xs text-zinc-500"><summary className="cursor-pointer">Package details</summary><p className="mt-1 break-all">SHA-256 {project.sha256}<br/><a href={project.path} download className="text-blue-600 underline">Download verified catalog file</a></p></details></>}
      {storage && <SdCardNotice device={device} bytes={storage.bytes} />}
      {firmwareTooOld && <p className="text-xs text-amber-600">Update the base firmware to v{requiredFirmware} or newer before loading this project.</p>}
      {project.id !== "none" && <ProjectSettingsForm project={project as ProjectDefinition} value={config} disabled={busy} onChange={onConfig} />}
      {project.id === "board-control-api" && <p className="text-xs text-zinc-500">Generate a token, test requests and read the full guide in the Board API tab.</p>}
      {project.id === "none" && <p className="text-xs text-zinc-500">Restore the original firmware display while keeping your board settings. No additional file is required.</p>}
      {saved?.project === project.id && <p role="status" className="text-xs text-emerald-600">{saved.message}</p>}
      <div className="flex flex-wrap gap-2">
        {project.id !== "none" && device.activeProject === project.id && <Button disabled={busy || loading || stopping} onClick={onSave} className="h-10 rounded-xl border border-zinc-300 px-4 text-sm">Save changes</Button>}
        <Button disabled={disabled || firmwareTooOld || (project.id !== "none" && !modern) || ((project as Partial<ProjectDefinition>).abi ?? 0) > device.projectApi || (project.id === "none" && device.activeProject === "none")} onClick={onInstall} className={accentButton + " h-10 px-4"}>{device.activeProject === project.id ? project.id === "none" ? "Active project" : "Save & reinstall" : project.id === "none" ? "Restore default display" : `Save & load ${project.name}`}</Button>
      </div>
    </TabPanel>
  );
}

function megabytes(bytes: number) {
  return bytes >= 1024 ** 3 ? `${(bytes / 1024 ** 3).toFixed(1)} GB` : `${(bytes / 1024 ** 2).toFixed(1)} MB`;
}

/** Mirrors the board's own check; the board re-mounts the card and decides when installation starts. */
function SdCardNotice({ device, bytes }: { device: PublicDevice; bytes: number }) {
  const needed = bytes + 1024 * 1024;
  const warn = "rounded-xl bg-amber-50 p-3 text-xs text-amber-700 dark:bg-amber-950 dark:text-amber-300";
  if (device.projectApi < 3) return <p className={warn}>This project keeps its images and videos on a microSD card. Update the base firmware in Devices → Details → Firmware to add SD card project support.</p>;
  if (!device.sdCard?.mounted) return <p className={warn}>This project installs {megabytes(bytes)} of files onto a microSD card, and the board has not reported a card. Insert a FAT32 card; the board checks again when you install and refuses without one.</p>;
  if (device.sdCard.free < needed) return <p className={warn}>The board’s SD card reported {megabytes(device.sdCard.free)} free, but this project needs {megabytes(needed)}. Free up space or use a larger card; files from an identical earlier installation are reused.</p>;
  return <p className="text-xs text-emerald-600">SD card ready · {megabytes(device.sdCard.free)} free of {megabytes(device.sdCard.total)}. The base firmware and project flash stay untouched by these files.</p>;
}

function UploadPanel({
  busy,
  disabled,
  modern,
  file,
  fileName,
  upload,
  xhr,
  onFile,
  onUpload,
}: {
  busy: boolean;
  disabled: boolean;
  modern: boolean;
  file: File | null;
  fileName: string;
  upload: number | null;
  xhr: RefObject<XMLHttpRequest | null>;
  onFile: (file: File | undefined) => void;
  onUpload: () => void;
}) {
  const [dragging, setDragging] = useState(false);
  const choose = (selected?: File) => {
    setDragging(false);
    if (!busy) onFile(selected);
  };
  return (
    <section className={cardClass + " space-y-3 p-4"}>
      <h3 className="text-sm font-semibold">Upload a project file</h3>
      <p id="project-file-help" className="text-xs text-zinc-500">Choose one self-contained project ELF. No companion files are needed. Maximum 128 KB. Projects with SD card images and videos are installed from the catalog. The file contents are validated after selection.</p>
      <label
        className={`${dragging ? "border-blue-500 bg-blue-50 dark:bg-blue-950" : "border-zinc-300 dark:border-zinc-700"} ${busy ? "cursor-not-allowed opacity-50" : "cursor-pointer hover:border-blue-400"} flex min-h-28 items-center justify-center rounded-xl border-2 border-dashed p-4 text-center transition`}
        onDragEnter={(event) => { event.preventDefault(); if (!busy) setDragging(true); }}
        onDragOver={(event) => event.preventDefault()}
        onDragLeave={(event) => { if (!event.currentTarget.contains(event.relatedTarget as Node | null)) setDragging(false); }}
        onDrop={(event) => { event.preventDefault(); choose(event.dataTransfer.files[0]); }}
      >
        <input
          aria-label="Choose project file"
          aria-describedby="project-file-help"
          type="file"
          disabled={busy}
          className="sr-only"
          onChange={(event) => {
            const selected = event.currentTarget.files?.[0];
            event.currentTarget.value = "";
            choose(selected);
          }}
        />
        <span className="text-xs"><span className="block font-medium text-blue-600">Browse for a project file</span><span className="mt-1 block text-zinc-500">or drop it here · .elf · up to 128 KB</span></span>
      </label>
      {fileName && <div className="flex items-center justify-between gap-3 rounded-lg bg-emerald-50 p-3 text-xs dark:bg-emerald-950"><p className="min-w-0 break-words text-emerald-700 dark:text-emerald-300">Ready: {fileName}</p><Button disabled={busy} onClick={() => choose()} className="shrink-0 text-zinc-500 underline">Remove</Button></div>}
      {upload !== null && (
        <div role="status" className="text-xs">
          Uploading to console: {upload}%
          <progress value={upload} max={100} className="w-full" />
          <Button onClick={() => xhr.current?.abort()} className="mt-2 underline">Stop upload</Button>
        </div>
      )}
      <Button disabled={disabled || !modern || !file} onClick={onUpload} className={accentButton + " h-10 px-4"}>Upload and load</Button>
    </section>
  );
}

function HistoryPanel({
  device,
  installation,
  disabled,
  busy,
  confirmClear,
  onClear,
  onConfirmClear,
  onCancelClear,
  onReinstall,
}: {
  device: PublicDevice;
  installation: DeviceCommand | undefined;
  disabled: boolean;
  busy: boolean;
  confirmClear: boolean;
  onClear: () => void;
  onConfirmClear: () => void;
  onCancelClear: () => void;
  onReinstall: (id: string) => void;
}) {
  const completed = device.commands.some((c) => c.type.startsWith("project_") && !projectPending(c));
  const history = device.commands.filter((c) => c.type === "project_install");
  return (
    <>
      {completed && <Button disabled={busy} onClick={onClear} className="h-10 w-full rounded-xl border border-zinc-300 text-xs">Clear completed history and logs · all projects</Button>}
      <ConfirmDialog
        open={confirmClear}
        onClose={onCancelClear}
        onConfirm={onConfirmClear}
        title="Clear installation history?"
        confirmLabel="Clear"
        description="Removes completed installation logs and history for every project on this board. Cannot be undone."
      />
      {history.length > 1 && (
        <details className={cardClass + " p-4"}>
          <summary className="cursor-pointer text-xs">Installation history</summary>
          {history.filter((c) => c.id !== installation?.id).reverse().map((c) => (
            <div key={c.id} className="mt-3 flex items-center justify-between gap-3 text-xs">
              <div className="min-w-0">
                <p>{new Date(c.createdAt).toLocaleString()} · {c.transfer?.name ?? new URLSearchParams(c.arg).get("id")} · {c.transfer?.phase ?? c.status}</p>
                <p className="text-zinc-500">{c.result}</p>
              </div>
              {c.status === "done" && <Button disabled={disabled} onClick={() => onReinstall(c.id)} className="shrink-0 text-blue-600 underline">Reinstall</Button>}
            </div>
          ))}
        </details>
      )}
    </>
  );
}
