"use client";

import { Button, Tab, TabGroup, TabList, TabPanel, TabPanels } from "@headlessui/react";
import { useSession } from "next-auth/react";
import { useEffect, useRef, useState } from "react";
import { WeatherProjectSettings } from "@/components/WeatherProjectSettings";
import { AuthCard } from "@/components/AuthCard";
import { Select } from "@/components/Select";
import { ErrorText, accentButton, cardClass, inputClass } from "@/components/ui";
import { api, deviceName, isOtaActive } from "@/lib/device-client";
import { TIME_ZONES } from "@/lib/device-settings";
import { inspectProject } from "@/lib/project-file";
import { projectPending } from "@/lib/project-transfers";
import type { PublicDevice } from "@/lib/device-types";
import { DISPLAY_PROJECTS } from "@/lib/projects";
import { errorMessage } from "@/lib/esp";

const PROJECTS = [...DISPLAY_PROJECTS, { id: "none", name: "Default display", description: "Unload the project and return to the base display." }];

export function Projects({ active }: { active: boolean }) {
  const { data: session, status } = useSession();
  const user = session?.user;
  const [devices, setDevices] = useState<PublicDevice[] | null>(null);
  const [selected, setSelected] = useState("");
  const updates = useRef(0);
  const [error, setError] = useState<string | null>(null);
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
    const open = () => {
      source?.close();
      source = new EventSource("/api/devices/stream");
      source.addEventListener("devices", (event) => {
        updates.current += 1;
        setDevices(JSON.parse((event as MessageEvent).data));
      });
      // EventSource retries normal short-lived responses automatically, without warnings.
    };
    const onVisibility = () => {
      if (document.visibilityState === "visible") open();
      else source?.close();
    };
    onVisibility();
    document.addEventListener("visibilitychange", onVisibility);
    return () => {
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
      {devices === null && <p className="text-sm text-zinc-500">Loading boards…</p>}
      {devices?.length === 0 && <p className="rounded-xl border border-dashed border-zinc-300 p-6 text-sm text-zinc-500">Add a board in Devices to install a project.</p>}
      {device && <>
        <Select label="Board" value={device} options={devices ?? []} onChange={(d) => setSelected(d.id)} getKey={(d) => d.id} renderValue={(d) => `${deviceName(d)} · ${d.online ? "Online" : "Offline"}`} />
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
  const [lat, setLat] = useState(String(device.settings.weather_lat / 10000));
  const [lon, setLon] = useState(String(device.settings.weather_lon / 10000));
  const [unit, setUnit] = useState(device.settings.weather_unit);
  const [seconds, setSeconds] = useState(device.settings.project_seconds);
  const [timezone, setTimezone] = useState(String(device.settings.tz));
  const [file, setFile] = useState<File | null>(null);
  const [fileName, setFileName] = useState("");
  const [upload, setUpload] = useState<number | null>(null);
  const xhr = useRef<XMLHttpRequest | null>(null);
  const installation = device.commands.findLast((command) => command.type === "project_install" && (tab < PROJECTS.length ? new URLSearchParams(command.arg).get("id") === PROJECTS[tab].id : new URLSearchParams(command.arg).get("path")?.startsWith("/api/devices/")));
  const stopping = device.commands.some((command) => command.type === "project_stop" && projectPending(command));
  const loading = device.commands.some((command) => command.type === "project_install" && projectPending(command));
  const disabled = busy || loading || stopping || !device.projectSupported || isOtaActive(device.ota);
  const modern = device.firmware.localeCompare("1.0.8", undefined, { numeric: true }) >= 0;
  const run = async (action: () => Promise<void>) => {
    setBusy(true); setError(null); setSaved(null);
    try { await action(); } catch (err) { setError(errorMessage(err)); } finally { setBusy(false); }
  };
  const command = async (body: object) => {
    const result = await api<{ device: PublicDevice }>(`/api/devices/${device.id}/commands`, "POST", body);
    onUpdated(result.device);
  };
  const save = async (project: string) => {
    const latitude = Number(lat), longitude = Number(lon);
    if (project === "weather" && (!lat.trim() || !lon.trim() || !Number.isFinite(latitude) || !Number.isFinite(longitude) || Math.abs(latitude) > 90 || Math.abs(longitude) > 180)) throw new Error("Enter valid latitude and longitude.");
    const result = await api<{ device: PublicDevice }>(`/api/devices/${device.id}`, "PATCH", { settings: project === "weather" ? { weather_lat: Math.round(latitude * 10000), weather_lon: Math.round(longitude * 10000), weather_unit: unit } : { project_seconds: seconds, tz: timezone } });
    onUpdated(result.device); setSaved({ project, message: "Settings saved. Applied when the board connects." });
  };
  const install = (project: string) => run(async () => {
    if (project !== "none") await save(project);
    await command({ type: "project_install", project, latitude: Math.round(Number(lat) * 10000), longitude: Math.round(Number(lon) * 10000) });
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
    {!modern && <p className="rounded-xl bg-amber-50 p-3 text-xs text-amber-700 dark:bg-amber-950 dark:text-amber-300">Install base firmware v1.0.8 in Devices → Details → Firmware before loading a project. It uses internal executable memory and preserves installation diagnostics across a restart.</p>}
    <section className={cardClass + " flex items-center justify-between gap-3 p-4"}><div><p className="text-xs text-zinc-500">Active on display</p><p className="mt-1 text-sm font-semibold">{PROJECTS.find((p) => p.id === device.activeProject)?.name ?? device.activeProject}</p></div><span className={device.online ? "text-xs text-emerald-600" : "text-xs text-zinc-500"}>{device.online ? "Board online" : "Board offline"}</span></section>
    <ErrorText>{error}</ErrorText>
    {loading && !projectPending(installation) && <Button className="text-xs text-blue-600 underline" onClick={() => { const pending = device.commands.findLast((c) => c.type === "project_install" && projectPending(c)); const id = new URLSearchParams(pending?.arg).get("id"); const index = PROJECTS.findIndex((p) => p.id === id); setTab(index < 0 ? PROJECTS.length : index); }}>Another project is installing · open its log and controls</Button>}
    {installation && <section className={cardClass + " space-y-3 p-4"}>
      <div className="flex items-center justify-between gap-3"><h3 className="text-sm font-semibold">Installation · {transfer?.name ?? new URLSearchParams(installation.arg).get("id")}</h3><span className="text-xs capitalize">{transfer?.phase ?? installation.status}</span></div>
      <progress aria-label="Board download progress" value={transfer?.progress ?? 0} max={100} className="h-2 w-full accent-blue-600" />
      <p className="text-xs text-zinc-500">{transfer ? `${transfer.progress}% · ${transfer.bytes.toLocaleString()} / ${transfer.total.toLocaleString()} bytes${transfer.version ? ` · v${transfer.version}` : ""}` : "Waiting for installation report"}</p>
      {loading && device.firmware.localeCompare("1.0.5", undefined, { numeric: true }) < 0 && <p className="text-xs text-amber-600">Stopping on this older firmware restarts the board to interrupt its download.</p>}
      {loading && !device.online && <p role="status" className="text-xs text-amber-600">Board offline. Reconnect its power and Wi-Fi. You can cancel this request; waiting is limited.</p>}
      <div role="log" aria-label="Installation logs" aria-live="polite" className="max-h-64 overflow-auto rounded-lg bg-zinc-950 p-3 font-mono text-xs text-zinc-300">{logs.map((log) => <p key={log.seq} className={log.level === "error" ? "text-red-400" : ""}>{new Date(log.at).toLocaleTimeString()} · {log.message}</p>)}<div ref={logEnd} /></div>
      <ErrorText>{installation.status === "failed" && transfer?.phase !== "cancelled" ? installation.result : null}</ErrorText>
      <div className="flex gap-2">
        {projectPending(installation) && <Button disabled={busy} onClick={() => run(() => command({ type: "project_stop", command: installation.id }))} className={accentButton + " h-9 px-4"}>Stop installation</Button>}
        {!projectPending(installation) && installation.status === "failed" && <Button disabled={disabled} onClick={() => {
          const id = new URLSearchParams(installation.arg).get("id") ?? "none";
          if (!new URLSearchParams(installation.arg).get("path")?.startsWith("/api/devices/") && PROJECTS.some((p) => p.id === id)) install(id);
          else run(async () => { await command({ type: "project_install", retry: installation.id }); });
        }} className={accentButton + " h-9 px-4"}>Retry</Button>}
        <Button disabled={busy} onClick={() => run(async () => { const result = await api<{ devices: PublicDevice[] }>("/api/devices"); const current = result.devices.find((d) => d.id === device.id); if (current) onUpdated(current); })} className="h-9 px-3 text-xs text-zinc-500">Refresh status</Button>
      </div>
      {stopping && <p role="status" className="text-xs text-amber-600">Stop requested; waiting for board confirmation before another installation.</p>}
    </section>}
    <TabGroup selectedIndex={tab} onChange={(index) => { setTab(index); setSaved(null); setError(null); }}>
      <TabList className="flex gap-1 overflow-x-auto rounded-xl bg-zinc-100 p-1 dark:bg-zinc-800">{[...PROJECTS.map((p) => p.name), "Upload project"].map((name) => <Tab key={name} className="whitespace-nowrap rounded-lg px-4 py-2 text-xs font-medium outline-none data-selected:bg-white data-selected:shadow-sm data-focus:ring-2 data-focus:ring-blue-500 dark:data-selected:bg-zinc-900">{name}</Tab>)}</TabList>
      <TabPanels className="mt-4">{PROJECTS.map((project) => <TabPanel key={project.id} className={cardClass + " space-y-4 p-4"}>
        <div className="flex items-center justify-between"><h3 className="text-sm font-semibold">{project.name}</h3>{device.activeProject === project.id && <span className="text-xs text-emerald-600">Active on board</span>}</div>
        <p className="text-xs text-zinc-500">{project.description}</p>
        {"version" in project && <p className="text-xs text-zinc-500">v{project.version} · {(project.size / 1024).toFixed(1)} KB · <a href={project.path} download className="text-blue-600 underline">Download single project file</a></p>}
        {project.id === "weather" && <fieldset disabled={busy}><WeatherProjectSettings lat={lat} lon={lon} unit={unit} onCoordinates={(a,b) => { setLat(a); setLon(b); setSaved(null); }} onUnit={(value) => { setUnit(value); setSaved(null); }} /></fieldset>}
        {project.id === "analog-clock" && <fieldset disabled={busy}><ClockProjectSettings timezone={timezone} seconds={seconds} onTimezone={(value) => { setTimezone(value); setSaved(null); }} onSeconds={(value) => { setSeconds(value); setSaved(null); }} /></fieldset>}
        {project.id === "none" && <p className="text-xs text-zinc-500">Restore the original firmware display while keeping your board settings. No additional file is required.</p>}
        {saved?.project === project.id && <p role="status" className="text-xs text-emerald-600">{saved.message}</p>}
        <div className="flex flex-wrap gap-2">
          {project.id !== "none" && <Button disabled={busy || loading || stopping} onClick={() => run(() => save(project.id))} className={accentButton + " h-10 px-4"}>Save {project.name} settings</Button>}
          <Button disabled={disabled || (project.id !== "none" && !modern) || (project.id === "none" && device.activeProject === "none")} onClick={() => install(project.id)} className={accentButton + " h-10 px-4"}>{device.activeProject === project.id ? project.id === "none" ? "Active project" : "Reinstall project" : `Load ${project.name}`}</Button>
        </div>
      </TabPanel>)}<TabPanel>    <section className={cardClass + " space-y-3 p-4"}><h3 className="text-sm font-semibold">Upload a project file</h3><p className="text-xs text-zinc-500">Choose one .elf file containing its project identity. No companion files are needed. Maximum 128 KB.</p><input aria-label="Project file" type="file" accept=".elf" disabled={busy} className="w-full text-xs" onChange={async (e) => { const selected = e.target.files?.[0]; setFile(null); setFileName(""); setError(null); if (!selected) return; try { const meta = inspectProject(new Uint8Array(await selected.arrayBuffer())); setFile(selected); setFileName(`${meta.name} · v${meta.version} · ${selected.size.toLocaleString()} bytes`); } catch (err) { setError(errorMessage(err)); } }} />{fileName && <p className="text-xs text-zinc-500">{fileName}</p>}{upload !== null && <div role="status" className="text-xs">Uploading to console: {upload}%<progress value={upload} max={100} className="w-full" /><Button onClick={() => xhr.current?.abort()} className="mt-2 underline">Stop upload</Button></div>}<Button disabled={disabled || !modern || !file} onClick={uploadFile} className={accentButton + " h-10 px-4"}>Upload and load</Button></section>
</TabPanel></TabPanels>
    </TabGroup>
    {device.commands.some((c) => c.type.startsWith("project_") && !projectPending(c)) && <Button disabled={busy} onClick={() => run(async () => { const result = await api<{ device: PublicDevice }>(`/api/devices/${device.id}/projects`, "DELETE"); onUpdated(result.device); })} className="h-10 w-full rounded-xl border border-zinc-300 text-xs">Clear completed history and logs · all projects</Button>}
    {device.commands.filter((c) => c.type === "project_install").length > 1 && <details className={cardClass + " p-4"}><summary className="cursor-pointer text-xs">Installation history</summary>{device.commands.filter((c) => c.type === "project_install" && c.id !== installation?.id).reverse().map((c) => <div key={c.id} className="mt-3 text-xs"><p>{new Date(c.createdAt).toLocaleString()} · {c.transfer?.name ?? new URLSearchParams(c.arg).get("id")} · {c.transfer?.phase ?? c.status}</p><p className="text-zinc-500">{c.result}</p></div>)}</details>}
  </div>;
}

function ClockProjectSettings({ timezone, seconds, onTimezone, onSeconds }: { timezone: string; seconds: boolean; onTimezone: (value: string) => void; onSeconds: (value: boolean) => void }) {
  const [now, setNow] = useState<Date | null>(null);
  const [message, setMessage] = useState<string | null>(null);
  useEffect(() => { const tick = () => setNow(new Date()); tick(); const timer = setInterval(tick, 1000); return () => clearInterval(timer); }, []);
  const preview = now ? new Intl.DateTimeFormat("en-GB", { timeZone: timezone, hour: "2-digit", minute: "2-digit", ...(seconds ? { second: "2-digit" } : {}) }).format(now) : "—";
  return <div className="space-y-4">
    <div className="rounded-xl bg-zinc-100 p-4 text-center dark:bg-zinc-800"><p className="font-mono text-2xl tabular-nums">{preview}</p><p className="mt-1 text-xs text-zinc-500">Live preview · {timezone}</p></div>
    <label className="block space-y-1 text-xs">Board time zone<select value={timezone} onChange={(e) => onTimezone(e.target.value)} className={inputClass}>{TIME_ZONES.map((zone) => <option key={zone.id} value={zone.id}>{zone.label} · {zone.id}</option>)}</select></label>
    <Button onClick={() => { const zone = Intl.DateTimeFormat().resolvedOptions().timeZone; if (TIME_ZONES.some((entry) => entry.id === zone)) { onTimezone(zone); setMessage(`Selected ${zone}`); } else setMessage(`Your time zone is ${zone}. Select a supported board time zone from the list.`); }} className="text-xs text-blue-600 underline">Use my browser’s time zone</Button>
    {message && <p role="status" className="text-xs text-zinc-500">{message}</p>}
    <label className="flex items-center gap-2 text-xs"><input type="checkbox" checked={seconds} onChange={(e) => onSeconds(e.target.checked)} />Show second hand</label>
    <p className="text-xs text-zinc-500">Named time zones follow daylight-saving rules automatically. The board needs a network connection to synchronize time. These settings also control the board’s status-bar time.</p>
  </div>;
}
