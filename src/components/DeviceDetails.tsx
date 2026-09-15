"use client";

import { Button, Tab, TabGroup, TabList, TabPanel, TabPanels } from "@headlessui/react";
import { useState, type FormEvent, type ReactNode } from "react";
import { ConfirmDialog, ErrorText, Sheet, ToastBanner, accentButton, inputClass, secondaryButton, useToast } from "@/components/ui";
import {
  availableUpdate,
  boardName,
  deviceName,
  isOtaActive,
  recentSamples,
  sendCommand,
  signalLabel,
  timeAgo,
  uptime,
} from "@/lib/device-client";
import type { DeviceCommand, DeviceSample, PublicDevice, TestResult } from "@/lib/device-types";
import { errorMessage } from "@/lib/esp";
import { formatBytes } from "@/components/SdFileManager";

export type DetailsTab = "status" | "check" | "wifi" | "firmware" | "activity";
const TABS: { id: DetailsTab; label: string }[] = [
  { id: "status", label: "Status" },
  { id: "check", label: "HW check" },
  { id: "wifi", label: "Wi-Fi" },
  { id: "firmware", label: "Firmware" },
  { id: "activity", label: "Activity" },
];

const TESTS: { key: string; label: string }[] = [
  { key: "battery", label: "Battery" },
  { key: "memory", label: "Memory" },
  { key: "buttons", label: "Buttons" },
  { key: "led", label: "LED" },
  { key: "sd", label: "SD card" },
  { key: "codec", label: "Codec" },
  { key: "mic", label: "Mic" },
  { key: "speaker", label: "Speaker" },
  { key: "wifi", label: "Wi-Fi" },
  { key: "ble", label: "BLE" },
];

export function DeviceDetails({
  device,
  initialTab,
  onClose,
}: {
  device: PublicDevice;
  initialTab: DetailsTab;
  onClose: () => void;
}) {
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState<string | null>(null);
  const [toast, setToast] = useToast();

  const run = async (key: string, body: Record<string, string>) => {
    setBusy(key);
    setError(null);
    try {
      await sendCommand(device, body);
      return true;
    } catch (err) {
      setError(errorMessage(err));
      return false;
    } finally {
      setBusy(null);
    }
  };

  return (
    <Sheet
      open
      onClose={onClose}
      title={deviceName(device)}
      subtitle={`${boardName(device.board)} · ${device.online ? "Online" : `Offline, last seen ${timeAgo(device.lastSeen)}`}`}
    >
      <TabGroup defaultIndex={Math.max(0, TABS.findIndex((t) => t.id === initialTab))} className="space-y-4">
        <TabList className="-mx-1 flex gap-1 overflow-x-auto px-1">
          {TABS.map((tab) => (
            <Tab
              key={tab.id}
              className="h-8 shrink-0 rounded-lg px-3 text-xs font-medium text-zinc-500 outline-none data-focus:ring-2 data-focus:ring-zinc-900/20 data-hover:bg-zinc-100 data-selected:bg-zinc-900 data-selected:text-white dark:data-hover:bg-zinc-800 dark:data-selected:bg-white dark:data-selected:text-zinc-900"
            >
              {tab.label}
            </Tab>
          ))}
        </TabList>
        <ErrorText>{error}</ErrorText>
        <TabPanels>
          <TabPanel>
            <StatusPanel device={device} />
          </TabPanel>
          <TabPanel>
            <CheckPanel device={device} busy={busy} onRun={(test) => run(`test:${test}`, { type: "test", test })} />
          </TabPanel>
          <TabPanel>
            <WifiPanel device={device} busy={busy} run={run} onDone={setToast} />
          </TabPanel>
          <TabPanel>
            <FirmwarePanel device={device} busy={busy} onUpdate={(version) => run("ota", { type: "ota", version })} />
          </TabPanel>
          <TabPanel>
            <ActivityPanel commands={device.commands} />
          </TabPanel>
        </TabPanels>
      </TabGroup>
      <ToastBanner toast={toast} />
    </Sheet>
  );
}

function StatusPanel({ device }: { device: PublicDevice }) {
  const recent = recentSamples(device.samples, 6);
  return (
    <div className="space-y-4">
      <dl className="grid grid-cols-2 gap-2 text-sm">
        <Info label="Battery" value={device.battery >= 0 ? `${device.battery}%${device.charging ? " · charging" : ""}` : "—"} />
        <Info label="Signal" value={signalLabel(device.rssi)} />
        <Info label="Uptime" value={device.uptime ? uptime(device.uptime) : "—"} />
        <Info label="Free memory" value={device.heap ? `${Math.round(device.heap / 1024)} KB` : "—"} />
        <Info label="IP address" value={device.ip || "—"} />
        <Info label="MAC" value={device.mac} />
        <Info label="SD card" value={!device.sdCard ? "Not reported" : device.sdCard.mounted ? `${formatBytes(device.sdCard.free)} free` : "No card"} />
        <Info label="SD capacity" value={device.sdCard?.mounted ? `${formatBytes(device.sdCard.total)} · ${Math.round(((device.sdCard.total - device.sdCard.free) / Math.max(1, device.sdCard.total)) * 100)}% used` : "—"} />
      </dl>
      {device.sdCard?.mounted && (
        <div aria-label="SD card usage" className="h-2 overflow-hidden rounded-full bg-zinc-100 dark:bg-zinc-800">
          <div className="h-full rounded-full bg-blue-500" style={{ width: `${Math.min(100, ((device.sdCard.total - device.sdCard.free) / Math.max(1, device.sdCard.total)) * 100)}%` }} />
        </div>
      )}
      <p className="text-xs font-semibold tracking-wide text-zinc-400 uppercase">Last 6 hours</p>
      {recent.length < 2 ? (
        <p className="text-sm text-zinc-500">Charts appear after a few minutes online.</p>
      ) : (
        <div className="space-y-3">
          <Sparkline label="Battery" unit="%" samples={recent} pick={(s) => (s.battery >= 0 ? s.battery : null)} min={0} max={100} />
          <Sparkline label="Signal" unit=" dBm" samples={recent} pick={(s) => s.rssi || null} min={-95} max={-30} />
          <Sparkline label="Free memory" unit=" KB" samples={recent} pick={(s) => (s.heap ? Math.round(s.heap / 1024) : null)} />
        </div>
      )}
    </div>
  );
}

function Info({ label, value }: { label: string; value: string }) {
  return (
    <div className="min-w-0 rounded-xl bg-zinc-50 px-3 py-2 dark:bg-zinc-800/60">
      <dt className="text-xs text-zinc-500">{label}</dt>
      <dd className="truncate font-medium">{value}</dd>
    </div>
  );
}

/** Minimal line chart: latest value, min–max range and the trend. */
function Sparkline({
  label,
  unit,
  samples,
  pick,
  min,
  max,
}: {
  label: string;
  unit: string;
  samples: DeviceSample[];
  pick: (sample: DeviceSample) => number | null;
  min?: number;
  max?: number;
}) {
  const points = samples.map((s) => ({ t: s.t, v: pick(s) })).filter((p): p is { t: number; v: number } => p.v !== null);
  if (points.length < 2) return null;
  const values = points.map((p) => p.v);
  const lo = min ?? Math.min(...values);
  const hi = Math.max(max ?? Math.max(...values), lo + 1);
  const t0 = points[0].t;
  const span = Math.max(1, points.at(-1)!.t - t0);
  const width = 300;
  const height = 48;
  const path = points
    .map((p, i) => {
      const x = ((p.t - t0) / span) * width;
      const y = height - 2 - ((Math.min(hi, Math.max(lo, p.v)) - lo) / (hi - lo)) * (height - 4);
      return `${i ? "L" : "M"}${x.toFixed(1)},${y.toFixed(1)}`;
    })
    .join("");
  const last = values.at(-1)!;

  return (
    <figure className="rounded-xl bg-zinc-50 p-3 dark:bg-zinc-800/60">
      <figcaption className="flex items-baseline justify-between text-xs text-zinc-500">
        <span>{label}</span>
        <span>
          <span className="text-sm font-semibold text-zinc-900 tabular-nums dark:text-zinc-100">
            {last}
            {unit}
          </span>{" "}
          <span className="tabular-nums">
            ({Math.min(...values)}–{Math.max(...values)})
          </span>
        </span>
      </figcaption>
      <svg viewBox={`0 0 ${width} ${height}`} preserveAspectRatio="none" className="mt-2 h-12 w-full text-blue-500" role="img" aria-label={`${label} trend, now ${last}${unit}`}>
        <path d={path} fill="none" stroke="currentColor" strokeWidth="2" vectorEffect="non-scaling-stroke" strokeLinejoin="round" />
      </svg>
    </figure>
  );
}

const STATUS_STYLE: Record<TestResult["status"], string> = {
  idle: "bg-zinc-300 dark:bg-zinc-600",
  running: "bg-amber-400 animate-pulse",
  ok: "bg-emerald-500",
  fail: "bg-red-500",
  info: "bg-blue-500",
};
const STATUS_LABEL: Record<TestResult["status"], string> = {
  idle: "Idle",
  running: "Running",
  ok: "OK",
  fail: "Fail",
  info: "Info",
};

function CheckPanel({ device, busy, onRun }: { device: PublicDevice; busy: string | null; onRun: (test: string) => void }) {
  const running = Object.values(device.tests).some((t) => t.status === "running");
  return (
    <div className="space-y-3">
      <div className="flex items-center justify-between gap-3">
        <p className="text-xs text-zinc-500">
          {device.testsUpdatedAt ? `Updated ${timeAgo(device.testsUpdatedAt)}` : "No results yet"}
          {" · "}Buttons need someone at the device.
        </p>
        <Button
          onClick={() => onRun("all")}
          disabled={!device.online || running || busy !== null}
          className={accentButton + " h-9 shrink-0 px-3"}
        >
          {running ? "Running…" : "Run all"}
        </Button>
      </div>
      <ul className="divide-y divide-zinc-200 rounded-xl border border-zinc-200 dark:divide-zinc-800 dark:border-zinc-800">
        {TESTS.map((test) => {
          const result = device.tests[test.key] ?? { status: "idle", detail: "" };
          return (
            <li key={test.key} className="flex items-center gap-3 px-3 py-2.5 text-sm">
              <span className={`size-2 shrink-0 rounded-full ${STATUS_STYLE[result.status]}`} aria-hidden />
              <span className="w-20 shrink-0 font-medium">{test.label}</span>
              <span className="w-14 shrink-0 text-xs text-zinc-500">{STATUS_LABEL[result.status]}</span>
              <span className="min-w-0 flex-1 truncate text-xs text-zinc-500">{result.detail || "—"}</span>
              <Button
                onClick={() => onRun(test.key)}
                disabled={!device.online || running || busy !== null}
                className="rounded-lg px-2 py-1 text-xs font-medium text-zinc-500 data-disabled:opacity-40 data-hover:bg-zinc-100 dark:data-hover:bg-zinc-800"
              >
                Run
              </Button>
            </li>
          );
        })}
      </ul>
    </div>
  );
}

function WifiPanel({
  device,
  busy,
  run,
  onDone,
}: {
  device: PublicDevice;
  busy: string | null;
  run: (key: string, body: Record<string, string>) => Promise<boolean>;
  onDone: (message: string) => void;
}) {
  const [ssid, setSsid] = useState("");
  const [password, setPassword] = useState("");
  const [confirmForget, setConfirmForget] = useState(false);
  const [primary, backup] = device.networks;
  const pending = device.commands.findLast((c) => c.type.startsWith("wifi") && (c.status === "queued" || c.status === "sent"));

  const forget = async () => {
    setConfirmForget(false);
    if (await run("wifi_forget", { type: "wifi_forget" })) onDone("Backup network will be forgotten when the device applies it.");
  };

  const add = async (event: FormEvent) => {
    event.preventDefault();
    if (await run("wifi", { type: "wifi_add", ssid, password })) {
      setSsid("");
      setPassword("");
    }
  };

  return (
    <div className="space-y-4">
      <dl className="grid grid-cols-2 gap-2 text-sm">
        <Info label="Main network" value={primary || "—"} />
        <Info label="Backup network" value={backup || "—"} />
      </dl>
      <p className="text-xs text-zinc-500">
        The device switches to the backup network when the main one is out of reach for 15 seconds, for example when
        you take it to another place. Change the main network on the device’s setup page.
      </p>
      {backup && (
        <Button
          onClick={() => setConfirmForget(true)}
          disabled={busy !== null}
          className={secondaryButton + " h-10 w-full"}
        >
          Forget backup network
        </Button>
      )}
      <ConfirmDialog
        open={confirmForget}
        onClose={() => setConfirmForget(false)}
        onConfirm={forget}
        title="Forget backup network?"
        confirmLabel="Forget"
        description={`${backup} is removed from ${deviceName(device)}. The device falls back to the main network only.`}
      />
      <form onSubmit={add} className="space-y-3 rounded-xl bg-zinc-50 p-3 dark:bg-zinc-800/60">
        <p className="text-sm font-medium">{backup ? "Replace backup network" : "Add backup network"}</p>
        <input value={ssid} onChange={(e) => setSsid(e.target.value)} placeholder="Network name" maxLength={32} required className={inputClass} />
        <input
          type="password"
          value={password}
          onChange={(e) => setPassword(e.target.value)}
          placeholder="Password (empty for open networks)"
          maxLength={63}
          autoComplete="off"
          className={inputClass}
        />
        <Button type="submit" disabled={busy !== null || !ssid} className={accentButton + " h-10 w-full"}>
          Save to device
        </Button>
      </form>
      {pending && <p className="text-xs text-amber-600 dark:text-amber-400">Waiting for the device to confirm…</p>}
    </div>
  );
}

function FirmwarePanel({
  device,
  busy,
  onUpdate,
}: {
  device: PublicDevice;
  busy: string | null;
  onUpdate: (version: string) => void;
}) {
  const update = availableUpdate(device);
  const ota = device.ota;
  const active = isOtaActive(ota);

  return (
    <div className="space-y-4">
      <dl className="grid grid-cols-2 gap-2 text-sm">
        <Info label="Installed" value={device.firmware ? `v${device.firmware}` : "—"} />
        <Info label="Latest" value={update ? `v${update.version}` : device.firmware ? "Up to date" : "—"} />
      </dl>

      {active && ota && (
        <div className="space-y-2" role="status" aria-live="polite">
          <div className="flex justify-between text-sm">
            <span className="font-medium">{ota.state === "queued" ? "Waiting for device…" : `Installing v${ota.version}`}</span>
            <span className="tabular-nums text-zinc-500">{ota.progress}%</span>
          </div>
          <div className="h-2 overflow-hidden rounded-full bg-zinc-200 dark:bg-zinc-800">
            <div className="h-full rounded-full bg-blue-500 transition-[width] duration-300" style={{ width: `${ota.progress}%` }} />
          </div>
          <p className="text-xs text-zinc-500">Keep the device powered. It restarts when the update is installed.</p>
        </div>
      )}
      {ota?.state === "failed" && !active && <ErrorText>Update failed: {ota.error || "unknown error"}</ErrorText>}
      {ota?.state === "done" && !update && <p className="text-sm text-emerald-600 dark:text-emerald-400">✓ v{ota.version} installed</p>}

      {update && !active && (
        <Button onClick={() => onUpdate(update.version)} disabled={!device.online || busy !== null} className={accentButton + " h-11 w-full"}>
          Update to v{update.version} over Wi-Fi
        </Button>
      )}
      <p className="text-xs text-zinc-500">
        Settings and the console link are kept. If the download or verification fails, the device keeps running the current version.
      </p>
    </div>
  );
}

const COMMAND_LABELS: Record<DeviceCommand["type"], string> = {
  project_install: "Project installation",
  project_stop: "Stop project installation",
  restart: "Restart",
  poweroff: "Power off",
  identify: "Identify",
  notify: "Message",
  test: "Hardware check",
  ota: "Firmware update",
  wifi_add: "Add backup network",
  wifi_forget: "Forget backup network",
  sd_list: "SD card: open folder",
  sd_download: "SD card: download",
  sd_upload: "SD card: upload",
  sd_delete: "SD card: delete",
  sd_mkdir: "SD card: new folder",
  sd_rename: "SD card: rename",
  sd_format: "SD card: format",
};

function ActivityPanel({ commands }: { commands: DeviceCommand[] }) {
  if (!commands.length) return <p className="text-sm text-zinc-500">No commands sent yet.</p>;
  return (
    <ul className="divide-y divide-zinc-200 rounded-xl border border-zinc-200 text-sm dark:divide-zinc-800 dark:border-zinc-800">
      {[...commands].reverse().map((command) => (
        <li key={command.id} className="flex items-center gap-3 px-3 py-2.5">
          <div className="min-w-0 flex-1">
            <p className="font-medium">
              {COMMAND_LABELS[command.type]}
              {command.type === "notify" && <span className="font-normal text-zinc-500"> “{command.arg}”</span>}
              {command.type === "test" && <span className="font-normal text-zinc-500"> · {command.arg}</span>}
              {command.type.startsWith("sd_") && command.type !== "sd_format" && <span className="font-normal text-zinc-500"> · {new URLSearchParams(command.arg).get("path")}</span>}
            </p>
            <p className="truncate text-xs text-zinc-500">
              {timeAgo(command.createdAt)}
              {command.result && ` · ${command.result}`}
            </p>
          </div>
          <CommandBadge status={command.status} />
        </li>
      ))}
    </ul>
  );
}

function CommandBadge({ status }: { status: DeviceCommand["status"] }) {
  const styles: Record<DeviceCommand["status"], [string, ReactNode]> = {
    queued: ["bg-zinc-100 text-zinc-600 dark:bg-zinc-800 dark:text-zinc-300", "Queued"],
    sent: ["bg-amber-100 text-amber-700 dark:bg-amber-950 dark:text-amber-300", "Sent"],
    done: ["bg-emerald-100 text-emerald-700 dark:bg-emerald-950 dark:text-emerald-300", "Done"],
    failed: ["bg-red-100 text-red-700 dark:bg-red-950 dark:text-red-300", "Failed"],
  };
  const [className, label] = styles[status];
  return <span className={`shrink-0 rounded-md px-1.5 py-0.5 text-[11px] font-medium ${className}`}>{label}</span>;
}
