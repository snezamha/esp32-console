import type { DeviceSettings } from "@/lib/device-settings";

/**
 * Commands the console can send. The firmware receives `cmd.<id>.<type>=<url-encoded arg>`.
 *   restart, poweroff   —
 *   identify            beep and flash the LED ring
 *   notify              arg: text shown on the screen
 *   test                arg: test key or "all"
 *   ota                 arg: `path=<image path>&version=<version>&size=<bytes>`
 *   wifi_add            arg: `ssid=<ssid>&password=<password>` (saved as the backup network)
 *   wifi_forget         forget the backup network
 */
export type CommandType = "restart" | "poweroff" | "identify" | "notify" | "test" | "ota" | "wifi_add" | "wifi_forget" | "project_install";

export type DeviceCommand = {
  id: string;
  type: CommandType;
  arg: string;
  /** queued → sent (delivered, waiting for the board's ack) → done | failed */
  status: "queued" | "sent" | "done" | "failed";
  result: string;
  createdAt: number;
  updatedAt: number;
};

export type TestResult = { status: "idle" | "running" | "ok" | "fail" | "info"; detail: string };

export type OtaStatus = {
  state: "queued" | "downloading" | "done" | "failed";
  progress: number;
  error: string;
  version: string;
  updatedAt: number;
};

export type DeviceSample = { t: number; battery: number; rssi: number; heap: number };

/** A device as the panel sees it (no token). */
export type PublicDevice = {
  id: string;
  name: string;
  mac: string;
  board: string;
  firmware: string;
  createdAt: number;
  lastSeen: number;
  ip: string;
  rssi: number;
  battery: number;
  charging: boolean;
  heap: number;
  uptime: number;
  /** Bumped on every console settings edit. */
  rev: number;
  /** Board settings including console edits that are still on their way. */
  settings: DeviceSettings;
  activeProject: DeviceSettings["project"];
  projectSupported: boolean;
  /** False until the board uploaded its settings; until then `settings` are defaults. */
  settingsReported: boolean;
  commands: DeviceCommand[];
  tests: Record<string, TestResult>;
  testsUpdatedAt: number;
  /** One sample per minute, oldest first. */
  samples: DeviceSample[];
  ota: OtaStatus | null;
  /** Saved Wi-Fi networks: [primary, backup]. */
  networks: string[];
  online: boolean;
  /** Console edits have not reached the board yet. */
  syncing: boolean;
};
