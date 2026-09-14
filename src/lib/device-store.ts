import { randomBytes, randomInt, randomUUID } from "node:crypto";
import type { Device as DeviceRow, Pairing as PairingRow, Prisma } from "@prisma/client";
import { db } from "@/lib/db";
import { DEFAULT_SETTINGS, type DeviceSettings } from "@/lib/device-settings";
import type {
  CommandType,
  DeviceCommand,
  DeviceSample,
  OtaStatus,
  PublicDevice,
  TestResult,
} from "@/lib/device-types";

/** Pairing code lifetime; the board asks for a new one when it expires. */
export const CODE_TTL_MS = 10 * 60 * 1000;
/** A device counts as online when it synced within this window (a poll can be held up to 25 s). */
export const ONLINE_WINDOW_MS = 60 * 1000;
const COMMAND_TTL_MS = 10 * 60 * 1000;
const SAMPLE_INTERVAL_MS = 60 * 1000;
const MAX_SAMPLES = 24 * 60;
const MAX_COMMANDS = 20;
/** How often a held request re-checks the database while waiting for a change. */
const POLL_INTERVAL_MS = 800;
/** Commands the board confirms with an explicit ack; the rest are done once delivered. */
const ACKED_COMMANDS: CommandType[] = ["test", "ota", "wifi_add", "wifi_forget"];

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
    settings: effectiveSettings(row),
    settingsReported: row.settingsReported,
    commands: (row.commands as DeviceCommand[]).slice(-10),
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
  const out: Record<string, unknown> = { ...(row.reported as object) };
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
  tests: Record<string, TestResult>;
  ota: Pick<OtaStatus, "state" | "progress" | "error"> | null;
  networks: string[] | null;
  acks: { id: string; ok: boolean; result: string }[];
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
    if (pending[key]!.rev <= report.rev) delete pending[key];
  }
  data.pending = json(pending);

  if (report.settings) {
    data.reported = json(report.settings);
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

  let commands = row.commands as DeviceCommand[];
  let commandsChanged = false;
  if (report.acks.length) {
    commands = commands.map((c) => {
      const ack = report.acks.find((a) => a.id === c.id);
      if (!ack) return c;
      commandsChanged = true;
      return { ...c, status: ack.ok ? ("done" as const) : ("failed" as const), result: ack.result, updatedAt: now.getTime() };
    });
  }
  commands = commands.map((c) => {
    if (c.status !== "queued" || now.getTime() - c.createdAt <= COMMAND_TTL_MS) return c;
    commandsChanged = true;
    return { ...c, status: "failed" as const, result: "Device did not come online", updatedAt: now.getTime() };
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
    return { ...c, status: ACKED_COMMANDS.includes(c.type) ? ("sent" as const) : ("done" as const), updatedAt: Date.now() };
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
  patch: { name?: string; settings?: DeviceSettings },
): Promise<PublicDevice | null> {
  const owned = await db.device.findFirst({ where: { id, owner }, select: { id: true } });
  if (!owned) return null;

  const row = await mutateDevice(id, (current) => {
    if (current.owner !== owner) return null;
    const data: Prisma.DeviceUpdateInput = {};
    if (patch.name !== undefined && patch.name !== current.name) data.name = patch.name;
    if (patch.settings) {
      const currentSettings = effectiveSettings(current);
      const pending = { ...(current.pending as PendingEdits) };
      let changed = false;
      for (const key of Object.keys(patch.settings) as (keyof DeviceSettings)[]) {
        if (patch.settings[key] === currentSettings[key]) continue;
        pending[key] = { value: patch.settings[key], rev: current.rev + 1 };
        changed = true;
      }
      if (changed) {
        data.pending = json(pending);
        data.rev = current.rev + 1;
      }
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
): Promise<PublicDevice | null> {
  const owned = await db.device.findFirst({ where: { id, owner }, select: { id: true } });
  if (!owned) return null;
  const now = Date.now();

  const row = await mutateDevice(id, (current) => {
    if (current.owner !== owner) return null;
    let commands = current.commands as DeviceCommand[];
    // A newer command of the same kind replaces one that has not been delivered yet.
    commands = commands.filter((c) => !(c.status === "queued" && c.type === type));
    commands = [
      ...commands,
      { id: randomBytes(4).toString("hex"), type, arg, status: "queued" as const, result: "", createdAt: now, updatedAt: now },
    ].slice(-MAX_COMMANDS);
    const data: Prisma.DeviceUpdateInput = { commands: json(commands) };
    if (type === "ota") {
      const ota: OtaStatus = { state: "queued", progress: 0, error: "", version: otaVersion, updatedAt: now };
      data.ota = json(ota);
    }
    return data;
  });
  return row ? toPublic(row) : null;
}

/** Removes the device. Its board loses the token on its next check-in and gets a new code. */
export async function removeDevice(owner: string, id: string): Promise<boolean> {
  const { count } = await db.device.deleteMany({ where: { id, owner } });
  return count > 0;
}

// ---------------------------------------------------------------------------
// Live updates for the panel (server-sent events)

/** Polls the owner's devices for a change (any row's `version`/count) and returns the fresh list. */
export async function waitForChange(owner: string, signature: string, ms: number, signal: AbortSignal): Promise<PublicDevice[]> {
  const deadline = Date.now() + ms;
  for (;;) {
    const devices = await listDevices(owner);
    if (rowsSignature(devices) !== signature || Date.now() >= deadline || signal.aborted) return devices;
    await sleep(Math.min(POLL_INTERVAL_MS, deadline - Date.now()), signal);
  }
}

export function rowsSignature(devices: PublicDevice[]) {
  return devices.map((d) => `${d.id}:${d.rev}:${d.lastSeen}:${d.commands.length}`).join(",");
}
