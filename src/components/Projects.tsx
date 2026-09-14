"use client";

import { Button } from "@headlessui/react";
import { useSession } from "next-auth/react";
import { useEffect, useState } from "react";
import { AuthCard } from "@/components/AuthCard";
import { Select } from "@/components/Select";
import { ErrorText, accentButton, cardClass, inputClass } from "@/components/ui";
import { api, deviceName, isOtaActive } from "@/lib/device-client";
import type { PublicDevice } from "@/lib/device-types";
import { DISPLAY_PROJECTS } from "@/lib/projects";
import { errorMessage } from "@/lib/esp";

const PROJECTS = [...DISPLAY_PROJECTS, { id: "none", name: "Default display", description: "Unload the project and return to the base display." }];

export function Projects({ active }: { active: boolean }) {
  const { data: session, status } = useSession();
  const user = session?.user;
  const [devices, setDevices] = useState<PublicDevice[] | null>(null);
  const [selected, setSelected] = useState("");
  const [error, setError] = useState<string | null>(null);
  useEffect(() => {
    if (!active || !user) return;
    const source = new EventSource("/api/devices/stream");
    source.addEventListener("devices", (event) => {
      setDevices(JSON.parse((event as MessageEvent).data));
      setError(null);
    });
    source.onerror = () => setError("Connection interrupted. Reconnecting…");
    return () => source.close();
  }, [active, user]);
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
        <ProjectPicker key={device.id} device={device} onUpdated={(updated) => setDevices((current) => current?.map((d) => d.id === updated.id ? updated : d) ?? [updated])} />
      </>}
    </div>
  );
}

function ProjectPicker({ device, onUpdated }: { device: PublicDevice; onUpdated: (device: PublicDevice) => void }) {
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [lat, setLat] = useState(String(device.settings.weather_lat / 10000));
  const [lon, setLon] = useState(String(device.settings.weather_lon / 10000));
  const installation = device.commands.findLast((command) => command.type === "project_install");
  const loading = busy || installation?.status === "queued" || installation?.status === "sent";
  const installError = installation?.status === "failed" ? installation.result : null;
  const disabled = loading || !device.projectSupported || isOtaActive(device.ota);
  const install = async (project: string) => {
    setBusy(true);
    setError(null);
    try {
      const latitude = Number(lat), longitude = Number(lon);
      if (project === "weather" && (!lat.trim() || !lon.trim() || !Number.isFinite(latitude) || !Number.isFinite(longitude) || Math.abs(latitude) > 90 || Math.abs(longitude) > 180)) throw new Error("Enter valid latitude (−90 to 90) and longitude (−180 to 180).");
      const body = project === "weather" ? { type: "project_install", project, latitude: Math.round(latitude * 10000), longitude: Math.round(longitude * 10000) } : { type: "project_install", project };
      const result = await api<{ device: PublicDevice }>(`/api/devices/${device.id}/commands`, "POST", body);
      onUpdated(result.device);
    } catch (err) { setError(errorMessage(err)); }
    finally { setBusy(false); }
  };
  return <div className="space-y-3">
    {!device.projectSupported && <p className="rounded-xl bg-amber-50 p-3 text-xs text-amber-700 dark:bg-amber-950 dark:text-amber-300">Update the board to base firmware v1.0.4 or later in Devices → Details → Firmware, then wait for it to reconnect.</p>}
    {loading && <div role="status" aria-live="polite" className="rounded-xl bg-blue-50 p-3 text-sm text-blue-700 dark:bg-blue-950 dark:text-blue-300"><span className="mr-2 inline-block size-3 animate-spin rounded-full border-2 border-current border-t-transparent" />{device.online ? "Loading project on the board…" : "Waiting for the board to come online…"}</div>}
    <ErrorText>{error || installError}</ErrorText>
    {PROJECTS.map((project) => {
      const current = device.activeProject === project.id;
      const unavailable = "board" in project && project.board !== device.board;
      const locationChanged = project.id === "weather" && (Math.round(Number(lat) * 10000) !== device.settings.weather_lat || Math.round(Number(lon) * 10000) !== device.settings.weather_lon);
      return <section key={project.id} className={cardClass + " space-y-3 p-4"}>
        <div className="flex items-center justify-between gap-3"><h3 className="text-sm font-semibold">{project.name}</h3>{current && <span className="text-xs font-medium text-emerald-600">Active</span>}</div>
        <p className="text-xs text-zinc-500">{project.description}</p>
        {project.id === "weather" && <>
          <div className="grid grid-cols-2 gap-2">
            <label className="space-y-1 text-xs text-zinc-500">Latitude<input type="number" min="-90" max="90" step="0.0001" value={lat} onChange={(e) => setLat(e.target.value)} disabled={disabled} className={inputClass} /></label>
            <label className="space-y-1 text-xs text-zinc-500">Longitude<input type="number" min="-180" max="180" step="0.0001" value={lon} onChange={(e) => setLon(e.target.value)} disabled={disabled} className={inputClass} /></label>
          </div>
          <p className="text-xs text-zinc-500">Default: Berlin. Weather data by <a href="https://open-meteo.com/" target="_blank" rel="noreferrer" className="underline">Open-Meteo</a>.</p>
        </>}
        <Button disabled={disabled || unavailable || (current && !locationChanged)} onClick={() => install(project.id)} className={accentButton + " h-10 w-full"}>{current ? locationChanged ? "Update location" : "Active project" : project.id === "none" ? "Restore default" : "Load project"}</Button>
      </section>;
    })}
  </div>;
}
