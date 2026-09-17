"use client";

import { Button } from "@headlessui/react";
import { useEffect, useRef, useState } from "react";
import { Group, inputClass, secondaryButton } from "@/components/ui";
import type { ProjectConfig, ProjectDefinition, ProjectSettingValue } from "@/lib/project-config";

type TestState = "loading" | "playing" | "error";

export function RadioProjectSettings({ project, value, onChange }: { project: ProjectDefinition; value: ProjectConfig; onChange: (value: ProjectConfig) => void }) {
  const set = (key: string, next: ProjectSettingValue) => onChange({ ...value, [key]: next });
  const byKey = (key: string) => project.settings.find((setting) => setting.key === key);
  const station = byKey("station");
  const audioRef = useRef<HTMLAudioElement | null>(null);
  const [testing, setTesting] = useState<{ slot: number; state: TestState } | null>(null);

  const stopTest = () => {
    if (audioRef.current) { audioRef.current.pause(); audioRef.current.src = ""; audioRef.current = null; }
    setTesting(null);
  };
  useEffect(() => stopTest, []);

  const testUrl = (slot: number, url: string) => {
    stopTest();
    setTesting({ slot, state: "loading" });
    const audio = new Audio();
    audioRef.current = audio;
    audio.oncanplay = () => setTesting({ slot, state: "playing" });
    audio.onerror = () => setTesting({ slot, state: "error" });
    audio.src = url;
    audio.play().catch(() => setTesting({ slot, state: "error" }));
  };

  return <fieldset className="space-y-5">
    {station && (
      <label className="block space-y-1 text-xs">
        {station.label}
        <select value={String(value[station.key] ?? station.default)} onChange={(event) => set(station.key, event.target.value)} className={inputClass}>
          {station.options?.map((option) => <option key={option.value} value={option.value}>{option.label}</option>)}
        </select>
      </label>
    )}
    {[1, 2, 3].map((slot) => {
      const urlSetting = byKey(`custom${slot}`);
      const nameSetting = byKey(`custom${slot}_name`);
      const langSetting = byKey(`custom${slot}_lang`);
      if (!urlSetting || !nameSetting || !langSetting) return null;
      const url = String(value[urlSetting.key] ?? "");
      const name = String(value[nameSetting.key] ?? "");
      const lang = String(value[langSetting.key] ?? langSetting.default);
      const urlValid = url === "" || /^https?:\/\//i.test(url);
      const status = testing?.slot === slot ? testing.state : null;
      const nameLimit = nameSetting.maxLength ?? 256;
      return (
        <Group key={slot} title={`Custom station ${slot}`}>
          <label className="block space-y-1 text-xs">
            {urlSetting.label}
            <div className="flex gap-2">
              <input type="text" value={url} maxLength={urlSetting.maxLength ?? 256} placeholder={urlSetting.placeholder}
                onChange={(event) => set(urlSetting.key, event.target.value)} className={inputClass} />
              <Button disabled={!url || !urlValid} onClick={() => (status === "playing" ? stopTest() : testUrl(slot, url))} className={secondaryButton + " h-11 shrink-0 px-3"}>
                {status === "playing" ? "Stop" : "Test"}
              </Button>
            </div>
          </label>
          {!urlValid && <p className="text-xs text-red-600 dark:text-red-400">Must start with https:// or http:// — the device drops this station silently otherwise.</p>}
          {status === "loading" && <p role="status" className="text-xs text-zinc-500">Loading…</p>}
          {status === "playing" && (
            <p role="status" className="text-xs text-emerald-600 dark:text-emerald-400">
              Playing — sounds good. <Button onClick={stopTest} className="underline">Stop</Button>
            </p>
          )}
          {status === "error" && <p role="status" className="text-xs text-red-600 dark:text-red-400">Couldn’t play this stream in the browser. It may still work on the device.</p>}
          <label className="block space-y-1 text-xs">
            {nameSetting.label}
            <input type="text" value={name} maxLength={nameLimit} placeholder={nameSetting.placeholder}
              onChange={(event) => set(nameSetting.key, nameSetting.asciiOnly ? event.target.value.replace(/[^\x20-\x7E]/g, "") : event.target.value)}
              className={inputClass} />
            <span className="block text-right text-xs text-zinc-400">{name.length}/{nameLimit}</span>
          </label>
          <label className="block space-y-1 text-xs">
            {langSetting.label}
            <select value={lang} onChange={(event) => set(langSetting.key, event.target.value)} className={inputClass}>
              {langSetting.options?.map((option) => <option key={option.value} value={option.value}>{option.label}</option>)}
            </select>
          </label>
        </Group>
      );
    })}
  </fieldset>;
}
