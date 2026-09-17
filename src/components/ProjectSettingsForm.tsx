"use client";

import { Button } from "@headlessui/react";
import { useEffect, useState } from "react";
import { RadioProjectSettings } from "@/components/RadioProjectSettings";
import { WeatherProjectSettings } from "@/components/WeatherProjectSettings";
import { Slider, Toggle, inputClass } from "@/components/ui";
import { TIME_ZONES } from "@/lib/device-settings";
import type { ProjectConfig, ProjectDefinition, ProjectLocation, ProjectSettingValue } from "@/lib/project-config";

export function ProjectSettingsForm({ project, value, disabled, onChange }: { project: ProjectDefinition; value: ProjectConfig; disabled: boolean; onChange: (value: ProjectConfig) => void }) {
  const set = (key: string, next: ProjectSettingValue) => onChange({ ...value, [key]: next });
  // Radio's 9 settings are 3 repeated url/name/lang triplets; a generic per-field list can't
  // group them into station cards or add stream-test/validation, so it gets its own renderer.
  if (project.id === "radio") return <fieldset disabled={disabled}><RadioProjectSettings project={project} value={value} onChange={onChange} /></fieldset>;
  return <fieldset disabled={disabled} className="space-y-4">
    {project.settings.map((setting) => {
      const current = value[setting.key] ?? setting.default;
      if (setting.type === "location") return <WeatherProjectSettings key={setting.key} value={current as ProjectLocation} onChange={(next) => set(setting.key, next)} />;
      if (setting.type === "boolean") return <Toggle key={setting.key} label={setting.label} checked={Boolean(current)} onChange={(next) => set(setting.key, next)} />;
      if (setting.type === "timezone") return <TimezoneSetting key={setting.key} label={setting.label} value={String(current)} onChange={(next) => set(setting.key, next)} />;
      if (setting.type === "select") return <label key={setting.key} className="block space-y-1 text-xs">{setting.label}<select value={String(current)} onChange={(event) => set(setting.key, event.target.value)} className={inputClass}>{setting.options?.map((option) => <option key={option.value} value={option.value}>{option.label}</option>)}</select></label>;
      if (setting.type === "number") return <Slider key={setting.key} label={setting.label} value={Number(current)} min={setting.min ?? 0} max={setting.max ?? 100} step={setting.step ?? 1} onChange={(next) => set(setting.key, next)} />;
      if (setting.type === "text") {
        // Filtered on every keystroke, not just on save, so asciiOnly rejects non-English
        // characters (the device's bitmap font can't render them) as soon as they're typed.
        const filter = (raw: string) => (setting.asciiOnly ? raw.replace(/[^\x20-\x7E]/g, "") : raw);
        return <label key={setting.key} className="block space-y-1 text-xs">{setting.label}<input type="text" value={String(current ?? "")} maxLength={setting.maxLength ?? 256} placeholder={setting.placeholder} onChange={(event) => set(setting.key, filter(event.target.value))} className={inputClass} /></label>;
      }
      return null;
    })}
  </fieldset>;
}

function TimezoneSetting({ label, value, onChange }: { label: string; value: string; onChange: (value: string) => void }) {
  const [now, setNow] = useState<Date | null>(null);
  const [message, setMessage] = useState("");
  useEffect(() => { const tick = () => setNow(new Date()); tick(); const timer = setInterval(tick, 1000); return () => clearInterval(timer); }, []);
  const preview = now ? new Intl.DateTimeFormat("en-GB", { timeZone: value, hour: "2-digit", minute: "2-digit", second: "2-digit" }).format(now) : "—";
  return <div className="space-y-3">
    <div className="rounded-xl bg-zinc-100 p-4 text-center dark:bg-zinc-800"><p className="font-mono text-2xl tabular-nums">{preview}</p><p className="mt-1 text-xs text-zinc-500">Live preview · {value}</p></div>
    <label className="block space-y-1 text-xs">{label}<select value={value} onChange={(event) => onChange(event.target.value)} className={inputClass}>{TIME_ZONES.map((zone) => <option key={zone.id} value={zone.id}>{zone.label} · {zone.id}</option>)}</select></label>
    <Button onClick={() => { const zone = Intl.DateTimeFormat().resolvedOptions().timeZone; if (TIME_ZONES.some((entry) => entry.id === zone)) { onChange(zone); setMessage(`Selected ${zone}`); } else setMessage(`${zone} is not available in this firmware.`); }} className="text-xs text-blue-600 underline">Use my browser’s time zone</Button>
    {message && <p role="status" className="text-xs text-zinc-500">{message}</p>}
  </div>;
}
