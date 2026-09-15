import { expireProjectCommands, projectPending, type ProjectTransfer } from "@/lib/project-transfers";
import { createHash, randomBytes, randomInt, randomUUID } from "node:crypto";
import type { Device as DeviceRow, Pairing as PairingRow, Prisma } from "@prisma/client";
import { db } from "@/lib/db";
import { DEFAULT_SETTINGS, type DeviceSettings } from "@/lib/device-settings";
import { projectConfigsWithDefaults, type ProjectConfigs } from "@/lib/project-config";
import type {
  CommandType,
  DeviceCommand,
  FileCommandType,
  FileJob,
  SdEntry,
  DeviceSample,
  OtaStatus,
  PublicDevice,
  SdCardStatus,
  TestResult,
} from "@/lib/device-types";

/** Pairing code lifetime; the board asks for a new one when it expires. */
export const CODE_TTL_MS = 10 * 60 * 1000;
/** A device counts as online when it synced within this window (a poll can be held up to 25 s). */
export const ONLINE_WINDOW_MS = 60 * 1000;
const COMMAND_TTL_MS = 10 * 60 * 1000;
const COMMAND_ACK_TIMEOUT_MS = 60 * 1000;
const OTA_ACK_TIMEOUT_MS = 10 * 60 * 1000;
const SAMPLE_INTERVAL_MS = 60 * 1000;
const MAX_SAMPLES = 24 * 60;
const MAX_COMMANDS = 20;
/** How often a held request re-checks the database while waiting for a change. */
const POLL_INTERVAL_MS = 1500;
/** Commands the board confirms with an explicit ack; the rest are done once delivered. */
const FILE_COMMANDS: FileCommandType[] = ["sd_mount", "sd_unmount", "sd_list", "sd_download", "sd_upload", "sd_delete", "sd_mkdir", "sd_rename", "sd_format"];
const ACKED_COMMANDS: CommandType[] = ["test", "ota", "wifi_add", "wifi_forget", "project_install", "project_stop", ...FILE_COMMANDS];
/** A delivered SD card operation without an answer after this long is treated as lost. */
const FILE_JOB_TIMEOUT_MS = 3 * 60 * 1000;
const FILE_TTL_MS = 30 * 60 * 1000;
export const MAX_DEVICE_FILE_BYTES = 4 * 1024 * 1024;

type PendingEdits = Partial<Record<keyof DeviceSettings, { value: DeviceSettings[keyof DeviceSettings]; rev: number }>>;
const json = (value: unknown) => value as Prisma.InputJsonValue;

function sleep(ms: number, signal: AbortSignal) {
  return new Promise<void>((resolve) => {
    const timer = setTimeout(resolve, ms);
    signal.addEventListener("abort", () => {
      clearTimeout(timer);
      resolve();
    });
  });
}

function toPublic(row: DeviceRow): PublicDevice {
  const pending = row.pending as PendingEdits;
  const settings = effectiveSettings(row);
  return {
    id: row.id,
    name: row.name,
    mac: row.mac,
    board: row.board,
    firmware: row.firmware,
    createdAt: row.createdAt.getTime(),
    lastSeen: row.lastSeen.getTime(),
    ip: row.ip,
    rssi: row.rssi,
    battery: row.battery,
    charging: row.charging,
    heap: row.heap,
    uptime: row.uptime,
    rev: row.rev,
    settings,
    projectSettings: projectConfigsWithDefaults(row.projectSettings, settings),
    activeProject: (row.reported as DeviceSettings).project ?? "none",
    projectSupported: row.board === "esp32-s3-lcd-0.85" && Number((row.reported as Record<string, unknown>)._project_api) >= 1,
    projectApi: Number((row.reported as Record<string, unknown>)._project_api) || 0,
    activeProjectVersion: String((row.reported as Record<string, unknown>)._project_version ?? ""),
    activeProjectSha256: String((row.reported as Record<string, unknown>)._project_sha256 ?? ""),
    projectSafeMode: (row.reported as Record<string, unknown>)._project_safe === 1,
    sdCard: ((row.reported as Record<string, unknown>)._sd as SdCardStatus | undefined) ?? null,
    settingsReported: row.settingsReported,
    commands: expireProjectCommands(row.commands as DeviceCommand[]).slice(-10).map((entry) => { const command = { ...entry }; delete command.fileId; return command; }),
    tests: row.tests as Record<string, TestResult>,
    testsUpdatedAt: row.testsUpdatedAt?.getTime() ?? 0,
    samples: row.samples as DeviceSample[],
    ota: row.ota as OtaStatus | null,
    networks: row.networks as string[],
    online: Date.now() - row.lastSeen.getTime() < ONLINE_WINDOW_MS,
    syncing: Object.keys(pending).length > 0,
  };
}

/** Settings as the console sees them: what the board reported plus edits still on their way. */
function effectiveSettings(row: DeviceRow): DeviceSettings {
  const out: Record<string, unknown> = { ...DEFAULT_SETTINGS, ...(row.reported as object) };
  for (const [key, entry] of Object.entries(row.pending as PendingEdits)) out[key] = entry!.value;
  return out as DeviceSettings;
}

/**
 * Reads the device, lets `mutate` compute the fields to change, and writes them back guarded by
 * the row's version (optimistic concurrency — Prisma's `pg` adapter has no simple `SELECT ... FOR
 * UPDATE`, so a lost-update race is instead detected and retried here). Returns null if the
 * device does not exist; returns the unchanged row if `mutate` finds nothing to change.
 */
async function mutateDevice(
  id: string,
  mutate: (row: DeviceRow) => Prisma.DeviceUpdateInput | null,
): Promise<DeviceRow | null> {
  for (let attempt = 0; attempt < 6; attempt++) {
    const row = await db.device.findUnique({ where: { id } });
    if (!row) return null;
    const data = mutate(row);
    if (!data) return row;
    const { count } = await db.device.updateMany({
      where: { id, version: row.version },
      data: { ...data, version: { increment: 1 } },
    });
    if (count === 1) return db.device.findUniqueOrThrow({ where: { id } });
  }
  throw new Error("Device row is busy; try again.");
}

// ---------------------------------------------------------------------------
// Board sync

export type BoardReport = {
  secret: string;
  token: string;
  /** Code the board currently shows, so a waiting request knows the board is up to date. */
  code: string;
  mac: string;
  board: string;
  firmware: string;
  ip: string;
  rssi: number;
  battery: number;
  charging: boolean;
  heap: number;
  uptime: number;
  /** Last console rev the board applied. */
  rev: number;
  /** The board's current settings, already sanitized. */
  settings: DeviceSettings | null;
  projectApi: number;
  projectVersion: string;
  projectSha256: string;
  projectSafeMode: boolean;
  sdCard: SdCardStatus | null;
  tests: Record<string, TestResult>;
  ota: Pick<OtaStatus, "state" | "progress" | "error"> | null;
  networks: string[] | null;
  acks: { id: string; ok: boolean; result: string }[];
  projectStatus: { id: string; phase: ProjectTransfer["phase"]; progress: number; bytes: number; total: number } | null;
  projectLogs: { id: string; seq: number; level: "info" | "error"; message: string }[];
  resetReason: string;
  unlink: boolean;
};

export type SyncResult =
  | { status: "pending"; code: string; expiresIn: number }
  | { status: "linked"; token: string; name: string; rev: number; settings: Partial<DeviceSettings>; commands: DeviceCommand[] }
  | { status: "unlinked" };

/** Applies telemetry/settings/test/ack fields from a report onto a device row. */
function applyReport(row: DeviceRow, report: BoardReport, now: Date): Prisma.DeviceUpdateInput {
  const data: Prisma.DeviceUpdateInput = {
    lastSeen: now,
    ip: report.ip,
    rssi: report.rssi,
    battery: report.battery,
    charging: report.charging,
    heap: report.heap,
    uptime: report.uptime,
  };
  if (report.firmware) data.firmware = report.firmware;

  const pending = { ...(row.pending as PendingEdits) };
  for (const key of Object.keys(pending) as (keyof DeviceSettings)[]) {
    if (key === "project" && report.projectApi >= 1) { delete pending[key]; continue; } // Clear obsolete built-in selections after upgrade.
    const projectConfirmed = key !== "project" || report.settings?.project === pending[key]!.value;
    if (pending[key]!.rev <= report.rev && projectConfirmed) delete pending[key];
  }
  data.pending = json(pending);

  if (report.settings) {
    const settings: Partial<DeviceSettings> = { ...report.settings };
    if (report.projectApi < 1) delete settings.project;
    data.reported = json({ ...settings,
      weather_lat: (row.reported as DeviceSettings).weather_lat ?? DEFAULT_SETTINGS.weather_lat,
      weather_lon: (row.reported as DeviceSettings).weather_lon ?? DEFAULT_SETTINGS.weather_lon,
      weather_unit: (row.reported as DeviceSettings).weather_unit ?? "celsius",
      project_seconds: (row.reported as DeviceSettings).project_seconds ?? true,
      _project_api: report.projectApi,
      _project_version: report.projectVersion,
      _project_sha256: report.projectSha256,
      _project_safe: report.projectSafeMode ? 1 : 0,
      ...(report.sdCard ? { _sd: report.sdCard } : {}),
    });
    data.settingsReported = true;
  }

  if (Object.keys(report.tests).length) {
    data.tests = json(report.tests);
    data.testsUpdatedAt = now;
  }
  if (report.networks) data.networks = json(report.networks);
  if (report.ota) {
    const previous = row.ota as OtaStatus | null;
    const ota: OtaStatus = { ...report.ota, version: previous?.version ?? "", updatedAt: now.getTime() };
    data.ota = json(ota);
  }

  let commands = expireProjectCommands(row.commands as DeviceCommand[], now.getTime());
  let commandsChanged = commands.some((command, index) => command !== (row.commands as DeviceCommand[])[index]);
  commands = commands.map((command) => {
    if (command.type !== "project_install") return command;
    if (!projectPending(command)) {
      if (command.transfer?.phase !== "cancelled") return command;
      const fresh = report.projectLogs.filter((log) => log.id === command.id && !command.transfer!.logs.some((entry) => entry.seq === log.seq)).map((log) => ({ seq: log.seq, level: log.level, message: log.message, at: now.getTime() }));
      const stop = commands.find((entry) => entry.type === "project_stop" && entry.arg === command.id);
      const ack = stop && report.acks.find((entry) => entry.id === stop.id);
      if (ack && projectPending(stop)) fresh.push({ seq: -now.getTime(), level: ack.ok ? "info" : "error", message: ack.result, at: now.getTime() });
      if (!fresh.length) return command;
      commandsChanged = true;
      return { ...command, transfer: { ...command.transfer, logs: [...command.transfer.logs, ...fresh].slice(-200) } };
    }
    const logs = report.projectLogs.filter((log) => log.id === command.id);
    const status = report.projectStatus?.id === command.id ? report.projectStatus : null;
    const base = command.transfer ?? { phase: command.status, progress: 0, bytes: 0, total: 0, logs: [], name: "Project", version: "" };
    if (!status && !logs.length && !(command.status === "sent" && report.uptime < row.uptime)) return command;
    commandsChanged = true;
    if (command.status === "sent" && report.uptime < row.uptime && !report.acks.some((ack) => ack.id === command.id)) {
      const result = `Board restarted during installation (${report.resetReason || "unknown reset reason"}). Retry the project upload.`;
      return { ...command, status: "failed" as const, result, updatedAt: now.getTime(), transfer: { ...base, phase: "failed" as const, logs: [...base.logs, { seq: -now.getTime(), at: now.getTime(), level: "error" as const, message: result }] } };
    }
    const fresh = logs.filter((log) => !base.logs.some((entry) => entry.seq === log.seq)).map((log) => ({ seq: log.seq, level: log.level, message: log.message, at: now.getTime() }));
    return { ...command, updatedAt: now.getTime(), transfer: { ...base, ...(status ? { phase: status.phase, progress: status.progress, bytes: status.bytes, total: status.total } : {}), logs: [...base.logs, ...fresh].slice(-200) } };
  });
  if (report.acks.length) {
    commands = commands.map((c) => {
      const ack = report.acks.find((a) => a.id === c.id);
      if (!ack || !projectPending(c)) return c;
      commandsChanged = true;
      return { ...c, status: ack.ok ? ("done" as const) : ("failed" as const), result: ack.result, updatedAt: now.getTime(), transfer: c.transfer ? { ...c.transfer, phase: ack.ok ? "done" as const : "failed" as const, progress: ack.ok ? 100 : c.transfer.progress, logs: c.transfer.logs.some((entry) => entry.message === ack.result) ? c.transfer.logs : [...c.transfer.logs, { seq: -now.getTime(), at: now.getTime(), level: ack.ok ? "info" as const : "error" as const, message: ack.result }].slice(-200) } : undefined };
    });
  }
  commands = commands.map((c) => {
    const queuedExpired = c.status === "queued" && now.getTime() - c.createdAt > COMMAND_TTL_MS;
    const sentFor = now.getTime() - c.updatedAt;
    const sentExpired = c.status === "sent" && c.type !== "project_install" && (
      (FILE_COMMANDS.includes(c.type as FileCommandType) && sentFor > FILE_JOB_TIMEOUT_MS) ||
      (c.type === "ota" && sentFor > OTA_ACK_TIMEOUT_MS) ||
      (c.type !== "ota" && !FILE_COMMANDS.includes(c.type as FileCommandType) && sentFor > COMMAND_ACK_TIMEOUT_MS)
    );
    if (!queuedExpired && !sentExpired) return c;
    commandsChanged = true;
    return { ...c, status: "failed" as const, result: queuedExpired ? "Device did not come online before the request expired." : "The board did not acknowledge the command before its timeout.", updatedAt: now.getTime() };
  });
  if (commandsChanged) data.commands = json(commands);

  const samples = row.samples as DeviceSample[];
  const last = samples.at(-1);
  if (!last || now.getTime() - last.t >= SAMPLE_INTERVAL_MS) {
    const next = [...samples, { t: now.getTime(), battery: report.battery, rssi: report.rssi, heap: report.heap }].slice(-MAX_SAMPLES);
    data.samples = json(next);
  }
  return data;
}

type Delivery = { settings: Partial<DeviceSettings>; commands: DeviceCommand[]; data: Prisma.DeviceUpdateInput | null };

/** What to deliver right now (pending settings, queued commands) — queued commands are flipped
 * to sent/done in `data`, so a caller that writes `data` back marks them as delivered. */
function pickDelivery(row: DeviceRow): Delivery {
  const settings: Record<string, unknown> = {};
  for (const [key, entry] of Object.entries(row.pending as PendingEdits)) settings[key] = entry!.value;

  const source = row.commands as DeviceCommand[];
  const toDeliver: DeviceCommand[] = [];
  const updated = source.map((c) => {
    if (c.status !== "queued") return c;
    toDeliver.push(c);
    return { ...c, status: ACKED_COMMANDS.includes(c.type) ? ("sent" as const) : ("done" as const), updatedAt: Date.now(), transfer: c.transfer ? { ...c.transfer, phase: "sent" as const, logs: [...c.transfer.logs, { seq: -Date.now(), at: Date.now(), level: "info" as const, message: "Board accepted the installation request." }] } : undefined };
  });

  return { settings: settings as Partial<DeviceSettings>, commands: toDeliver, data: toDeliver.length ? { commands: json(updated) } : null };
}

function idle(delivery: Delivery) {
  return delivery.commands.length === 0 && Object.keys(delivery.settings).length === 0;
}

/**
 * Board check-in. Holds the request open (re-checking the database every ~800 ms) for up to
 * `waitMs` when there is nothing to deliver yet, so console edits reach the board almost as soon
 * as they are made without the board having to poll aggressively.
 */
export async function syncBoard(report: BoardReport, waitMs: number, signal: AbortSignal): Promise<SyncResult> {
  if (report.token) {
    const found = await db.device.findUnique({ where: { token: report.token } });
    if (!found || report.unlink) {
      if (found) await db.device.delete({ where: { id: found.id } }).catch(() => {});
      return { status: "unlinked" };
    }
    const deviceId = found.id;

    const now = new Date();
    let row: DeviceRow = (await mutateDevice(deviceId, (current) => applyReport(current, report, now)))!;
    let delivery = pickDelivery(row);

    if (idle(delivery) && waitMs > 0) {
      const deadline = Date.now() + waitMs;
      while (Date.now() < deadline && !signal.aborted) {
        await sleep(Math.min(POLL_INTERVAL_MS, deadline - Date.now()), signal);
        const fresh = await db.device.findUnique({ where: { id: deviceId } });
        if (!fresh) return { status: "unlinked" };
        if (fresh.version === row.version) continue;
        row = fresh;
        delivery = pickDelivery(row);
        if (!idle(delivery)) break;
      }
    }

    if (delivery.data) {
      let final: Delivery = delivery;
      const written = await mutateDevice(deviceId, (current) => {
        final = pickDelivery(current);
        return final.data;
      });
      if (!written) return { status: "unlinked" };
      row = written;
      delivery = final;
    }

    return { status: "linked", token: row.token, name: row.name, rev: row.rev, settings: delivery.settings, commands: delivery.commands };
  }

  if (!/^[0-9a-f]{16,64}$/.test(report.secret)) return { status: "unlinked" };

  let pairing = await upsertPairing(report);
  if (waitMs > 0 && report.code === pairing.code) {
    const deadline = Date.now() + waitMs;
    while (Date.now() < deadline && !signal.aborted && !pairing.deviceId) {
      await sleep(Math.min(POLL_INTERVAL_MS, deadline - Date.now()), signal);
      const fresh = await db.pairing.findUnique({ where: { mac: report.mac } });
      if (!fresh || fresh.secret !== pairing.secret) break; // Expired or replaced under us.
      pairing = fresh;
    }
  }

  if (pairing.deviceId) {
    const device = await db.device.update({
      where: { id: pairing.deviceId },
      data: { lastSeen: new Date(), ip: report.ip, rssi: report.rssi },
    });
    await db.pairing.delete({ where: { mac: report.mac } }).catch(() => {});
    return { status: "linked", token: device.token, name: device.name, rev: device.rev, settings: {}, commands: [] };
  }

  return {
    status: "pending",
    code: pairing.code,
    expiresIn: Math.max(0, Math.round((pairing.expiresAt.getTime() - Date.now()) / 1000)),
  };
}

/** Creates or refreshes the pending pairing row for this board (one row per MAC address). */
async function upsertPairing(report: BoardReport): Promise<PairingRow> {
  const existing = await db.pairing.findUnique({ where: { mac: report.mac } });
  const now = Date.now();
  if (existing && existing.secret === report.secret && (existing.deviceId || existing.expiresAt.getTime() > now)) {
    return existing;
  }

  let code: string;
  do code = randomInt(0, 1_000_000).toString().padStart(6, "0");
  while (await db.pairing.findFirst({ where: { code, mac: { not: report.mac } } }));

  return db.pairing.upsert({
    where: { mac: report.mac },
    create: { mac: report.mac, secret: report.secret, code, board: report.board, firmware: report.firmware, expiresAt: new Date(now + CODE_TTL_MS) },
    update: { secret: report.secret, code, board: report.board, firmware: report.firmware, expiresAt: new Date(now + CODE_TTL_MS), deviceId: null },
  });
}

// ---------------------------------------------------------------------------
// Panel

export async function listDevices(owner: string): Promise<PublicDevice[]> {
  const rows = await db.device.findMany({ where: { owner }, orderBy: { createdAt: "asc" } });
  return rows.map(toPublic);
}

/** Links the board showing `code` to `owner`. Returns null when no board shows that code. */
export async function claimCode(owner: string, code: string): Promise<PublicDevice | null> {
  // Atomic claim: only one caller can win a given code. This placeholder is never a real device
  // id (those are UUIDs); it is replaced by the real one once the device row exists (below).
  const CLAIMING = "claiming";
  const { count } = await db.pairing.updateMany({ where: { code, deviceId: null }, data: { deviceId: CLAIMING } });
  if (count !== 1) return null;
  const pairing = await db.pairing.findFirst({ where: { code } });
  if (!pairing) return null; // Expired between the claim and this read; extremely unlikely.

  return db.$transaction(async (tx) => {
    // Re-adding a board replaces its previous record, whoever owned it.
    await tx.device.deleteMany({ where: { mac: pairing.mac } });
    const device = await tx.device.create({
      data: {
        id: randomUUID(),
        owner,
        token: randomBytes(24).toString("hex"),
        mac: pairing.mac,
        board: pairing.board,
        firmware: pairing.firmware,
        reported: json(DEFAULT_SETTINGS),
      },
    });
    await tx.pairing.update({ where: { mac: pairing.mac }, data: { deviceId: device.id } });
    return toPublic(device);
  });
}

export async function updateDevice(
  owner: string,
  id: string,
  patch: { name?: string; settings?: DeviceSettings; projectSettings?: { project: string; values: ProjectConfigs[string] } },
): Promise<PublicDevice | null> {
  const owned = await db.device.findFirst({ where: { id, owner }, select: { id: true } });
  if (!owned) return null;

  const row = await mutateDevice(id, (current) => {
    if (current.owner !== owner) return null;
    const data: Prisma.DeviceUpdateInput = {};
    const currentSettings = effectiveSettings(current);
    if (patch.name !== undefined && patch.name !== current.name) data.name = patch.name;
    if (patch.settings) {
      const pending = { ...(current.pending as PendingEdits) };
      let changed = false;
      for (const key of Object.keys(patch.settings) as (keyof DeviceSettings)[]) {
        if (patch.settings[key] === currentSettings[key]) continue;
        if (key === "weather_lat" || key === "weather_lon" || key === "weather_unit" || key === "project_seconds") {
          const reported = { ...(data.reported as object ?? current.reported as object), [key]: patch.settings[key] };
          data.reported = json(reported);
          continue;
        }
        pending[key] = { value: patch.settings[key], rev: current.rev + 1 };
        changed = true;
      }
      if (changed) {
        data.pending = json(pending);
        data.rev = current.rev + 1;
      }
    }
    if (patch.projectSettings) {
      const currentProjects = projectConfigsWithDefaults(current.projectSettings, currentSettings);
      data.projectSettings = json({ ...currentProjects, [patch.projectSettings.project]: patch.projectSettings.values });
    }
    return Object.keys(data).length ? data : null;
  });
  return row ? toPublic(row) : null;
}

export async function queueCommand(
  owner: string,
  id: string,
  type: CommandType,
  arg: string,
  otaVersion = "",
  project?: { name: string; version: string; size: number; bytes?: Buffer; fileId?: string },
): Promise<PublicDevice | null> {
  const owned = await db.device.findFirst({ where: { id, owner }, select: { id: true } });
  if (!owned) return null;
  const now = Date.now();
  let fileId = project?.fileId;
  let createdFile = false;
  if (project?.bytes) {
    const stored = await db.projectFile.create({
      data: { deviceId: id, name: project.name, version: project.version, bytes: Uint8Array.from(project.bytes) },
      select: { id: true },
    });
    fileId = stored.id;
    createdFile = true;
  }

  let row: DeviceRow | null;
  try { row = await mutateDevice(id, (current) => {
    if (current.owner !== owner) return null;
    let commands = expireProjectCommands(current.commands as DeviceCommand[]);
    if ((type === "project_install" || type === "ota") && commands.some((c) =>
      ((c.type === "project_install" || c.type === "ota") && c.status === "sent") || (c.type === "project_stop" && projectPending(c)))) {
      throw new Error("An installation is already running.");
    }
    // A newer command of the same kind replaces one that has not been delivered yet.
    commands = commands.filter((c) => !(c.status === "queued" && c.type === type));
    const commandId = randomBytes(4).toString("hex");
    if (fileId) {
      const values = new URLSearchParams(arg);
      values.set("path", `/api/devices/${id}/projects/${commandId}/file`);
      arg = values.toString();
    }
    commands = [
      ...commands,
      { id: commandId, type, arg, status: "queued" as const, result: "", createdAt: now, updatedAt: now,
        ...(type === "project_install" ? { transfer: { phase: "queued" as const, progress: 0, bytes: 0, total: project?.size ?? 0, name: project?.name ?? "Default display", version: project?.version ?? "", logs: [{ seq: -now, at: now, level: "info" as const, message: "Installation requested. Waiting for the board to accept the file." }] }, ...(fileId ? { fileId } : {}) } : {}),
      },
    ].slice(-MAX_COMMANDS);
    const data: Prisma.DeviceUpdateInput = { commands: json(commands) };
    if (type === "ota") {
      const ota: OtaStatus = { state: "queued", progress: 0, error: "", version: otaVersion, updatedAt: now };
      data.ota = json(ota);
    }
    return data;
  }); } catch (error) {
    if (createdFile && fileId) await db.projectFile.delete({ where: { id: fileId } }).catch(() => {});
    throw error;
  }
  if (!row) {
    if (createdFile && fileId) await db.projectFile.delete({ where: { id: fileId } }).catch(() => {});
    return null;
  }
  const retainedFiles = (row.commands as DeviceCommand[]).flatMap((command) => command.fileId ? [command.fileId] : []);
  await db.projectFile.deleteMany({ where: { deviceId: id, ...(retainedFiles.length ? { id: { notIn: retainedFiles } } : {}) } });
  return toPublic(row);
}

/** Removes the device. Its board loses the token on its next check-in and gets a new code. */
export async function removeDevice(owner: string, id: string): Promise<boolean> {
  const { count } = await db.device.deleteMany({ where: { id, owner } });
  return count > 0;
}

// ---------------------------------------------------------------------------
// Live updates for the panel (server-sent events)

/** A compact change token, so a held stream does not deserialize settings, logs and samples on
 * every database check. */
export async function deviceRowsSignature(owner: string) {
  const rows = await db.device.findMany({ where: { owner }, orderBy: { createdAt: "asc" }, select: { id: true, version: true } });
  return rows.map((row) => `${row.id}:${row.version}`).join(",");
}

/** Polls compact row versions and fetches full devices only after a change or timeout. */
export async function waitForChange(owner: string, signature: string, ms: number, signal: AbortSignal): Promise<PublicDevice[]> {
  const deadline = Date.now() + ms;
  for (;;) {
    if (await deviceRowsSignature(owner) !== signature || Date.now() >= deadline || signal.aborted) return listDevices(owner);
    await sleep(Math.min(POLL_INTERVAL_MS, deadline - Date.now()), signal);
  }
}

/** Project configuration stays in the console and is sent only as the active module's data payload. */
export async function projectSettingsForBoard(token: string, project: string): Promise<ProjectConfigs[string] | null> {
  if (!token) return null;
  const row = await db.device.findUnique({ where: { token } });
  if (!row) return null;
  return projectConfigsWithDefaults(row.projectSettings, effectiveSettings(row))[project] ?? null;
}

export async function stopProject(owner: string, id: string, target: string): Promise<PublicDevice | null> {
  const row = await mutateDevice(id, (current) => {
    if (current.owner !== owner) return null;
    const commands = expireProjectCommands(current.commands as DeviceCommand[]);
    const command = commands.find((entry) => entry.id === target && entry.type === "project_install");
    if (!command || !projectPending(command)) return null;
    const now = Date.now();
    const updated = commands.map((entry) => entry.id !== target ? entry : { ...entry, status: "failed" as const, result: "Cancelled by user", updatedAt: now, transfer: entry.transfer ? { ...entry.transfer, phase: "cancelled" as const, logs: [...entry.transfer.logs, { seq: -now, at: now, level: "info" as const, message: entry.status === "queued" ? "Queued request cancelled." : "Stop requested. Waiting for the board to stop the transfer." }] } : undefined });
    if (command.status === "sent") updated.push({ id: randomBytes(4).toString("hex"), type: current.firmware.localeCompare("1.0.5", undefined, { numeric: true }) >= 0 ? "project_stop" : "restart", arg: target, status: "queued", result: "", createdAt: now, updatedAt: now });
    return { commands: json(updated.slice(-MAX_COMMANDS)) };
  });
  return row?.owner === owner ? toPublic(row) : null;
}

export async function projectFileForBoard(id: string, commandId: string, token: string): Promise<Buffer | null> {
  const row = await db.device.findFirst({ where: { id, token } });
  const command = (row?.commands as DeviceCommand[] | undefined)?.find((command) => command.id === commandId && command.type === "project_install");
  if (!command?.fileId) return null;
  const file = await db.projectFile.findFirst({ where: { id: command.fileId, deviceId: id }, select: { bytes: true } });
  return file ? Buffer.from(file.bytes) : null;
}

export async function retryProjectFile(owner: string, id: string, target: string): Promise<PublicDevice | null> {
  const row = await db.device.findFirst({ where: { id, owner } });
  const command = (row?.commands as DeviceCommand[] | undefined)?.find((entry) => entry.id === target && entry.type === "project_install");
  if (!command?.fileId || !command.transfer || projectPending(command)) throw new Error("Choose the project file again to retry.");
  return queueCommand(owner, id, "project_install", command.arg, "", { name: command.transfer.name, version: command.transfer.version, size: command.transfer.total, fileId: command.fileId });
}

/** Deletes completed project requests/logs; leaves installed projects and pending work intact. */
export async function clearProjectHistory(owner: string, id: string): Promise<PublicDevice | null> {
  let deletedFileIds: string[] = [];
  const row = await mutateDevice(id, (current) => {
    if (current.owner !== owner) return null;
    const history = expireProjectCommands(current.commands as DeviceCommand[]);
    const commands = history.filter((command) => !command.type.startsWith("project_") || projectPending(command) || history.some((stop) => stop.type === "project_stop" && stop.arg === command.id && projectPending(stop)));
    deletedFileIds = history.filter((command) => !commands.includes(command) && command.fileId).map((command) => command.fileId!);
    return { commands: json(commands) };
  });
  if (row?.owner === owner && deletedFileIds.length) await db.projectFile.deleteMany({ where: { id: { in: deletedFileIds }, deviceId: id } });
  return row?.owner === owner ? toPublic(row) : null;
}

/** Removes one activity entry, or the complete activity list when `target` is omitted. */
export async function deleteCommandHistory(owner: string, id: string, target?: string): Promise<PublicDevice | null> {
  let removed: DeviceCommand[] = [];
  const row = await mutateDevice(id, (current) => {
    if (current.owner !== owner) return null;
    const commands = current.commands as DeviceCommand[];
    removed = target ? commands.filter((command) => command.id === target) : commands;
    return { commands: json(target ? commands.filter((command) => command.id !== target) : []) };
  });
  if (!row || row.owner !== owner) return null;

  const retainedProjectFiles = new Set((row.commands as DeviceCommand[]).flatMap((command) => command.fileId ? [command.fileId] : []));
  const projectFileIds = removed.flatMap((command) => command.fileId && !retainedProjectFiles.has(command.fileId) ? [command.fileId] : []);
  if (projectFileIds.length) await db.projectFile.deleteMany({ where: { deviceId: id, id: { in: projectFileIds } } });
  if (removed.length) await db.deviceFile.deleteMany({ where: { deviceId: id, id: { in: removed.map((command) => command.id) } } });
  return toPublic(row);
}

// ---------------------------------------------------------------------------
// SD card file manager

function fileJobPending(command: DeviceCommand, now = Date.now()) {
  return FILE_COMMANDS.includes(command.type as FileCommandType) && (command.status === "queued" || (command.status === "sent" && now - command.updatedAt < FILE_JOB_TIMEOUT_MS));
}

/** Queues one SD card operation. `upload` holds the bytes the board fetches for sd_upload. */
export async function queueFileCommand(owner: string, id: string, type: FileCommandType, params: Record<string, string>, upload?: Buffer): Promise<{ device: PublicDevice; job: string } | null> {
  const owned = await db.device.findFirst({ where: { id, owner }, select: { id: true } });
  if (!owned) return null;
  const now = Date.now();
  const job = randomBytes(4).toString("hex");
  const values = new URLSearchParams(params);
  if (type === "sd_list" || type === "sd_download" || type === "sd_upload") values.set("src", `/api/device/files/${job}`);
  if (upload) {
    values.set("size", String(upload.length));
    values.set("sha256", createHash("sha256").update(upload).digest("hex"));
  }
  await db.deviceFile.deleteMany({ where: { deviceId: id, createdAt: { lt: new Date(now - FILE_TTL_MS) } } });
  if (upload) await db.deviceFile.create({ data: { id: job, deviceId: id, bytes: Uint8Array.from(upload) } });
  let row: DeviceRow | null;
  try {
    row = await mutateDevice(id, (current) => {
      if (current.owner !== owner) return null;
      let commands = expireProjectCommands(current.commands as DeviceCommand[]);
      if (commands.some((command) => fileJobPending(command, now))) throw new Error("Another SD card operation is running. Try again when it finishes.");
      if (commands.some((command) => command.type === "project_install" && projectPending(command))) throw new Error("A project installation is using the SD card.");
      // File browsing creates many commands; keep only the latest few finished ones in the history.
      const finished = commands.filter((command) => FILE_COMMANDS.includes(command.type as FileCommandType) && !fileJobPending(command, now));
      const dropped = new Set(finished.slice(0, -4).map((command) => command.id));
      commands = commands
        .filter((command) => !dropped.has(command.id))
        .map((command) => FILE_COMMANDS.includes(command.type as FileCommandType) && command.status === "sent" && !fileJobPending(command, now) ? { ...command, status: "failed" as const, result: "The board did not answer.", updatedAt: now } : command);
      commands = [...commands, { id: job, type, arg: values.toString(), status: "queued" as const, result: "", createdAt: now, updatedAt: now }].slice(-MAX_COMMANDS);
      return { commands: json(commands) };
    });
  } catch (error) {
    if (upload) await db.deviceFile.delete({ where: { id: job } }).catch(() => {});
    throw error;
  }
  if (!row) {
    if (upload) await db.deviceFile.delete({ where: { id: job } }).catch(() => {});
    return null;
  }
  return { device: toPublic(row), job };
}

/** State of one SD card operation for its owner; listings are parsed once the board answered. */
export async function fileJob(owner: string, id: string, job: string): Promise<{ job: FileJob; bytes: Buffer | null; path: string } | null> {
  const row = await db.device.findFirst({ where: { id, owner } });
  const command = (row?.commands as DeviceCommand[] | undefined)?.find((entry) => entry.id === job && FILE_COMMANDS.includes(entry.type as FileCommandType));
  if (!row || !command) return null;
  const expired = command.status === "sent" && Date.now() - command.updatedAt >= FILE_JOB_TIMEOUT_MS;
  const status = expired ? "failed" : command.status;
  const result: FileJob = { id: job, type: command.type as FileCommandType, status, result: expired ? "The board did not answer." : command.result };
  let bytes: Buffer | null = null;
  if (status === "done" && (command.type === "sd_list" || command.type === "sd_download")) {
    const file = await db.deviceFile.findFirst({ where: { id: job, deviceId: id }, select: { bytes: true } });
    bytes = file ? Buffer.from(file.bytes) : null;
    if (command.type === "sd_list") result.entries = bytes ? parseListing(bytes.toString("utf8")) : [];
  }
  return { job: result, bytes, path: new URLSearchParams(command.arg).get("path") ?? "" };
}

function parseListing(text: string): SdEntry[] {
  return text.split("\n").flatMap((line) => {
    const [kind, size, modified, ...name] = line.split("\t");
    if ((kind !== "d" && kind !== "f") || !name.length) return [];
    return [{ name: name.join("\t"), folder: kind === "d", size: Number(size) || 0, modified: (Number(modified) || 0) * 1000 }];
  }).sort((a, b) => Number(b.folder) - Number(a.folder) || a.name.localeCompare(b.name));
}

/** Board side: the command must be a delivered transfer of this board. */
async function boardTransfer(token: string, job: string, types: FileCommandType[]) {
  if (!token) return null;
  const row = await db.device.findUnique({ where: { token } });
  const command = (row?.commands as DeviceCommand[] | undefined)?.find((entry) => entry.id === job && types.includes(entry.type as FileCommandType) && entry.status === "sent");
  return row && command ? { row, command } : null;
}

export async function uploadForBoard(token: string, job: string): Promise<Buffer | null> {
  const found = await boardTransfer(token, job, ["sd_upload"]);
  if (!found) return null;
  const file = await db.deviceFile.findFirst({ where: { id: job, deviceId: found.row.id }, select: { bytes: true } });
  return file ? Buffer.from(file.bytes) : null;
}

export async function storeBoardTransfer(token: string, job: string, bytes: Buffer): Promise<boolean> {
  const found = await boardTransfer(token, job, ["sd_list", "sd_download"]);
  if (!found || bytes.length > MAX_DEVICE_FILE_BYTES) return false;
  await db.deviceFile.upsert({ where: { id: job }, create: { id: job, deviceId: found.row.id, bytes: Uint8Array.from(bytes) }, update: { bytes: Uint8Array.from(bytes), createdAt: new Date() } });
  return true;
}
