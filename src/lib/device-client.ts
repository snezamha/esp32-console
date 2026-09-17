import { BOARDS } from "@/lib/boards";
import type { DeviceSample, OtaStatus, PublicDevice } from "@/lib/device-types";

/** JSON request to the console API; throws with the server's error message. */
export async function api<T>(url: string, method = "GET", body?: unknown, signal?: AbortSignal): Promise<T> {
  const res = await fetch(url, {
    method,
    cache: "no-store",
    signal,
    headers: body === undefined ? undefined : { "Content-Type": "application/json" },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const data = res.status === 204 ? {} : await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(data.error ?? `Request failed (${res.status})`);
  return data as T;
}

export async function sendCommand(device: PublicDevice, body: Record<string, string>) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), 20_000);
  try {
    return await api<{ device: PublicDevice }>(`/api/devices/${device.id}/commands`, "POST", body, controller.signal);
  } catch (error) {
    if (controller.signal.aborted) throw new Error("The request timed out. Check device activity before retrying; the command may still have reached the board.");
    throw error;
  } finally {
    clearTimeout(timer);
  }
}

export function boardName(id: string) {
  return BOARDS.find((b) => b.id === id)?.name ?? (id || "Unknown board");
}

export function deviceName(device: PublicDevice) {
  return device.name || boardName(device.board);
}

/** Newest firmware with an OTA image, if newer than what the device runs. */
export function availableUpdate(device: PublicDevice) {
  const latest = BOARDS.find((b) => b.id === device.board)?.versions.find((v) => v.app);
  return latest && device.firmware && compareVersions(latest.version, device.firmware) > 0 ? latest : null;
}

export function compareVersions(a: string, b: string) {
  const pa = a.split(/[.-]/).map((n) => parseInt(n, 10) || 0);
  const pb = b.split(/[.-]/).map((n) => parseInt(n, 10) || 0);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    if ((pa[i] ?? 0) !== (pb[i] ?? 0)) return (pa[i] ?? 0) - (pb[i] ?? 0);
  }
  return 0;
}

export function duration(seconds: number) {
  if (seconds <= 0) return "Never";
  return seconds < 60 ? `${seconds} s` : `${seconds / 60} min`;
}

export function timeAgo(timestamp: number) {
  const seconds = Math.max(0, Math.round((Date.now() - timestamp) / 1000));
  if (seconds < 60) return `${seconds}s ago`;
  if (seconds < 3600) return `${Math.round(seconds / 60)} min ago`;
  if (seconds < 86400) return `${Math.round(seconds / 3600)} h ago`;
  return `${Math.round(seconds / 86400)} d ago`;
}

export function uptime(seconds: number) {
  if (seconds < 3600) return `${Math.floor(seconds / 60)} min`;
  if (seconds < 86400) return `${Math.floor(seconds / 3600)} h ${Math.floor((seconds % 3600) / 60)} min`;
  return `${Math.floor(seconds / 86400)} d ${Math.floor((seconds % 86400) / 3600)} h`;
}

export function signalLabel(rssi: number) {
  if (!rssi) return "—";
  return `${rssi} dBm · ${rssi > -60 ? "Good" : rssi > -72 ? "Fair" : "Weak"}`;
}

/** An update that was queued or is downloading and has not gone quiet. */
export function isOtaActive(ota: OtaStatus | null): boolean {
  return !!ota && (ota.state === "queued" || ota.state === "downloading") && Date.now() - ota.updatedAt < 5 * 60 * 1000;
}

export function recentSamples(samples: DeviceSample[], hours: number) {
  const since = Date.now() - hours * 60 * 60 * 1000;
  return samples.filter((s) => s.t > since);
}

/** Measured change since the last charge-state transition, limited to the last six hours. */
export function batteryTrend(device: PublicDevice) {
  const samples = recentSamples(device.samples, 6).filter((s) => s.battery >= 0 && s.charging === device.charging);
  const lastTransition = device.samples.findLastIndex((s) => s.charging !== undefined && s.charging !== device.charging);
  const start = samples.find((s) => s.t > (device.samples[lastTransition]?.t ?? 0));
  if (!device.online || !start || device.battery < 0 || device.lastSeen - start.t < 5 * 60_000) return null;
  const minutes = Math.round((device.lastSeen - start.t) / 60_000);
  const percent = device.battery - start.battery;
  const millivolts = device.batteryMv > 0 && (start.batteryMv ?? 0) > 0 ? device.batteryMv - start.batteryMv! : null;
  const direction = millivolts !== null && Math.abs(millivolts) >= 20
    ? Math.sign(millivolts) : Math.abs(percent) >= 2 ? Math.sign(percent) : 0;
  return { minutes, percent, millivolts, direction };
}

/** Battery below this is flagged in the list. */
export const LOW_BATTERY = 20;
