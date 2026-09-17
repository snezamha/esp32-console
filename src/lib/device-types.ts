import type { ProjectTransfer } from "@/lib/project-transfers";
import type { DeviceSettings } from "@/lib/device-settings";
import type { ProjectConfigs } from "@/lib/project-config";

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
export type CommandType = "restart" | "poweroff" | "identify" | "notify" | "test" | "ota" | "wifi_add" | "wifi_forget" | "project_install" | "project_stop" | FileCommandType;

/**
 * SD card file manager (firmware 1.1.1+). Arguments are form-encoded: `path`, plus `to` (rename),
 * `src` (transfer URL on the console) and `size`/`sha256` (upload). See src/services/sd_files.h.
 */
export type FileCommandType = "sd_mount" | "sd_unmount" | "sd_list" | "sd_download" | "sd_upload" | "sd_delete" | "sd_mkdir" | "sd_rename" | "sd_format";
/** First firmware with the SD card file manager and the LED ring designer. */
export const SD_FILES_FIRMWARE = "1.1.1";
/** First firmware with explicit SD mount and unmount commands. */
export const SD_MOUNT_FIRMWARE = "1.1.7";
export type SdEntry = { name: string; folder: boolean; size: number; modified: number };
/** Board-measured work of a running SD operation: bytes for transfers, sectors for a format. */
export type FileProgress = { done: number; total: number };
export type FileJob = { id: string; type: FileCommandType; status: DeviceCommand["status"]; result: string; progress?: FileProgress; entries?: SdEntry[] };

export type DeviceCommand = {
  id: string;
  type: CommandType;
  arg: string;
  transfer?: ProjectTransfer;
  /** Last progress the board reported for a running SD operation. */
  progress?: FileProgress;
  /** Server-only reference to an uploaded binary; stripped from public responses. */
  fileId?: string;
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

export type SdCardStatus = { mounted: boolean; total: number; free: number };

export type DeviceSample = { t: number; battery: number; batteryMv?: number; charging?: boolean; rssi: number; heap: number };

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
  batteryMv: number;
  charging: boolean;
  heap: number;
  uptime: number;
  /** Bumped on every console settings edit. */
  rev: number;
  /** Board settings including console edits that are still on their way. */
  settings: DeviceSettings;
  projectSettings: ProjectConfigs;
  activeProject: DeviceSettings["project"];
  activeProjectVersion: string;
  activeProjectSha256: string;
  projectSafeMode: boolean;
  /** SD card as last checked by the board (boot, install attempt, menu mount); null on older firmware. */
  sdCard: SdCardStatus | null;
  projectApi: number;
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
