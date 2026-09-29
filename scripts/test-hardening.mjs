import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import ts from "typescript";

const compile = (source) =>
  ts.transpileModule(source, { compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2022 } }).outputText;
const load = (source) => import(`data:text/javascript;base64,${Buffer.from(source).toString("base64")}`);
const settings = await load(compile(readFileSync("src/lib/device-settings.ts", "utf8")));

// --- Who may sign in -------------------------------------------------------

const allowlist = await load(compile(readFileSync("src/lib/allowlist.ts", "utf8")));
process.env.AUTH_ALLOWED_EMAILS = "";
assert.equal(allowlist.allowlistConfigured(), false);
assert.equal(allowlist.emailAllowed("anyone@gmail.com"), true, "An unset list stays open, as documented");
process.env.AUTH_ALLOWED_EMAILS = " Owner@Gmail.com , @mycompany.com ";
assert.equal(allowlist.allowlistConfigured(), true);
assert.equal(allowlist.emailAllowed("owner@gmail.com"), true);
assert.equal(allowlist.emailAllowed("OWNER@GMAIL.COM"), true);
assert.equal(allowlist.emailAllowed("staff@mycompany.com"), true);
assert.equal(allowlist.emailAllowed("stranger@gmail.com"), false);
assert.equal(allowlist.emailAllowed("owner@gmail.com.evil.net"), false, "A domain entry must match the real domain");
assert.equal(allowlist.emailAllowed("evil.net@mycompany.com.attacker.io"), false);
assert.equal(allowlist.emailAllowed(""), false);
process.env.AUTH_ALLOWED_EMAILS = "";

// --- Shared rate-limit counters -------------------------------------------

let rows = new Map();
const rateDb = {
  rateLimit: {
    updateMany: async ({ where, data }) => {
      const row = rows.get(where.key);
      if (!row || !(row.windowStart < where.windowStart.lt)) return { count: 0 };
      rows.set(where.key, { ...row, ...data });
      return { count: 1 };
    },
    upsert: async ({ where, create, update }) => {
      const row = rows.get(where.key);
      const next = row ? { ...row, count: row.count + update.count.increment } : { ...create };
      rows.set(where.key, next);
      return next;
    },
    deleteMany: async ({ where }) => {
      if (where.key) rows.delete(where.key);
      return { count: 1 };
    },
  },
};
globalThis.__hardeningTest = { db: rateDb };
const rateLimit = await load(
  compile(readFileSync("src/lib/rate-limit.ts", "utf8")).replace(
    /import .* from "@\/lib\/db";/,
    "const { db } = globalThis.__hardeningTest;",
  ),
);

for (let attempt = 0; attempt < 3; attempt++) {
  assert.equal(await rateLimit.withinLimit("claim:attempts:owner", 3, 60_000), true);
}
assert.equal(await rateLimit.withinLimit("claim:attempts:owner", 3, 60_000), false, "The limit must hold across calls");
assert.equal(
  await rateLimit.withinLimit("claim:attempts:other", 3, 60_000),
  true,
  "One user's attempts must not spend another's budget",
);
await rateLimit.clearLimit("claim:attempts:owner");
assert.equal(await rateLimit.withinLimit("claim:attempts:owner", 3, 60_000), true, "A success clears the counter");
rows.set("claim:attempts:owner", { key: "claim:attempts:owner", count: 99, windowStart: new Date(Date.now() - 120_000) });
assert.equal(await rateLimit.withinLimit("claim:attempts:owner", 3, 60_000), true, "An expired window starts over");
// The counter is shared state, not a per-instance Map: a second "instance" sees the same rows.
const secondInstance = await load(
  compile(readFileSync("src/lib/rate-limit.ts", "utf8")).replace(
    /import .* from "@\/lib\/db";/,
    "const { db } = globalThis.__hardeningTest;",
  ),
);
rows.set("shared", { key: "shared", count: 2, windowStart: new Date() });
assert.equal(await secondInstance.withinLimit("shared", 2, 60_000), false, "Counters must be shared, not per instance");

// --- Device store: pairing throttle and Wi-Fi password handling ------------

const now = Date.now();
let pairing = {
  mac: "AA:BB:CC:DD:EE:FF",
  secret: "a".repeat(32),
  code: "111111",
  board: "esp32-s3-lcd-0.85",
  firmware: "1.1.27",
  expiresAt: new Date(now + 600_000),
  resetAt: new Date(now),
  deviceId: null,
};
let device = {
  id: "device-1",
  owner: "owner",
  token: "f".repeat(48),
  name: "Desk",
  mac: "AA:BB:CC:DD:EE:FF",
  board: "esp32-s3-lcd-0.85",
  firmware: "1.1.27",
  createdAt: new Date(now - 1000),
  lastSeen: new Date(now - 1000),
  ip: "",
  rssi: 0,
  battery: 50,
  batteryMv: 0,
  charging: false,
  heap: 0,
  uptime: 10,
  rev: 1,
  version: 1,
  reported: { ...settings.DEFAULT_SETTINGS },
  projectSettings: {},
  pending: {},
  settingsReported: true,
  commands: [
    {
      id: "aabbccdd",
      type: "wifi_add",
      arg: new URLSearchParams({ ssid: "Home", password: "super-secret-pass" }).toString(),
      status: "queued",
      result: "",
      createdAt: now,
      updatedAt: now,
    },
  ],
  tests: {},
  testsUpdatedAt: null,
  samples: [],
  ota: null,
  networks: [],
};

let upserts = 0;
const storeDb = {
  device: {
    findUnique: async ({ where, select }) => {
      const match = device && (where.id ? device.id === where.id : device.token === where.token);
      if (!match) return null;
      return select ? Object.fromEntries(Object.keys(select).map((key) => [key, device[key]])) : structuredClone(device);
    },
    findUniqueOrThrow: async () => structuredClone(device),
    findFirst: async () => structuredClone(device),
    findMany: async () => [structuredClone(device)],
    updateMany: async ({ where, data }) => {
      if (!device || device.id !== where.id || device.version !== where.version) return { count: 0 };
      device = { ...device, ...structuredClone(data), version: device.version + 1 };
      return { count: 1 };
    },
    update: async ({ data }) => {
      device = { ...device, ...structuredClone(data) };
      return structuredClone(device);
    },
    delete: async () => ({}),
  },
  pairing: {
    findUnique: async ({ where }) => (pairing && pairing.mac === where.mac ? structuredClone(pairing) : null),
    findFirst: async () => null,
    findMany: async () => [],
    upsert: async ({ update }) => {
      upserts++;
      pairing = { ...pairing, ...update };
      return structuredClone(pairing);
    },
    updateMany: async () => ({ count: 1 }),
    update: async ({ data }) => {
      pairing = { ...pairing, ...data };
      return structuredClone(pairing);
    },
    delete: async () => ({}),
  },
  deviceFile: { deleteMany: async () => ({ count: 0 }) },
  projectFile: { deleteMany: async () => ({ count: 0 }) },
  $transaction: async (run) => run(storeDb),
};
globalThis.__hardeningStore = { db: storeDb, DEFAULT_SETTINGS: settings.DEFAULT_SETTINGS };
const store = await load(
  compile(readFileSync("src/lib/device-store.ts", "utf8"))
    .replace(
      /import .* from "@\/lib\/project-transfers";/,
      "const expireProjectCommands = (commands) => commands; const projectPending = () => false;",
    )
    .replace(/import .* from "@\/lib\/db";/, "const { db } = globalThis.__hardeningStore;")
    .replace(/import .* from "@\/lib\/board-control";/, 'const DEFAULT_MUSIC_URL = "https://example.invalid/stream";')
    .replace(/import .* from "@\/lib\/device-settings";/, "const { DEFAULT_SETTINGS } = globalThis.__hardeningStore;")
    .replace(/import .* from "@\/lib\/project-config";/, "const projectConfigsWithDefaults = (raw) => raw;"),
);

const signal = new AbortController().signal;
const pairingReport = (secret) => ({
  secret,
  token: "",
  code: "",
  mac: pairing.mac,
  board: pairing.board,
  firmware: pairing.firmware,
  ip: "",
  rssi: 0,
  battery: -1,
  batteryMv: 0,
  charging: false,
  heap: 0,
  uptime: 0,
  rev: 0,
  settings: null,
  projectApi: 0,
  projectVersion: "",
  projectSha256: "",
  projectSafeMode: false,
  sdCard: null,
  tests: {},
  ota: null,
  networks: null,
  acks: [],
  projectStatus: null,
  projectLogs: [],
  fileProgress: null,
  resetReason: "",
  unlink: false,
});

// The board that owns this pairing keeps getting its own code back, for free.
const own = await store.syncBoard(pairingReport(pairing.secret), 0, signal);
assert.equal(own.status, "pending");
assert.equal(own.code, "111111");
assert.equal(upserts, 0, "An unchanged pairing must not be rewritten");

// A check-in that forges this MAC with a different secret is refused, and learns nothing.
const forged = await store.syncBoard(pairingReport("b".repeat(32)), 0, signal);
assert.equal(forged.status, "throttled", "A code cannot be churned by a foreign secret");
assert.equal(forged.code, undefined, "A throttled reply must never carry the live code");
assert.equal(pairing.code, "111111", "The board's shown code must survive the forged check-in");
assert.equal(upserts, 0);

// The same board after a genuine reboot, once the cooldown has passed, still gets a fresh code.
pairing = { ...pairing, resetAt: new Date(now - store.PAIRING_RESET_COOLDOWN_MS - 1000) };
const rebooted = await store.syncBoard(pairingReport("c".repeat(32)), 0, signal);
assert.equal(rebooted.status, "pending");
assert.equal(upserts, 1);
assert.notEqual(pairing.code, "111111", "A reboot past the cooldown replaces the code");
assert.equal(pairing.secret, "c".repeat(32));

// The Wi-Fi password reaches the board exactly once and is never handed to the browser.
assert.equal(
  new URLSearchParams((await store.listDevices("owner"))[0].commands[0].arg).get("password"),
  "",
  "A queued Wi-Fi password must not be sent to the browser",
);
const linked = await store.syncBoard(
  { ...pairingReport(""), token: device.token, mac: device.mac, uptime: 20, rev: device.rev, settings: device.reported },
  0,
  signal,
);
assert.equal(linked.status, "linked");
assert.equal(
  new URLSearchParams(linked.commands[0].arg).get("password"),
  "super-secret-pass",
  "The board still receives the password it needs to join",
);
assert.equal(
  new URLSearchParams(device.commands[0].arg).get("password"),
  "",
  "The stored row must keep no password after delivery",
);
assert.equal(new URLSearchParams(device.commands[0].arg).get("ssid"), "Home", "The network name stays in the history");

console.log("✓ Allowlist, shared rate limits, pairing throttle and Wi-Fi password redaction hold");
