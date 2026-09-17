import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import ts from "typescript";

const compile = (source) => ts.transpileModule(source, {
  compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2022 },
}).outputText;
const settings = await import(`data:text/javascript;base64,${Buffer.from(compile(readFileSync("src/lib/device-settings.ts", "utf8"))).toString("base64")}`);
const now = Date.now();
let device = {
  id: "existing", owner: "owner", token: "old-token", mac: "AA:BB:CC:DD:EE:FF", name: "Living room",
  board: "esp32-s3-lcd-0.85", firmware: "1.1.26", createdAt: new Date(now - 100_000),
  lastSeen: new Date(now - 100_000), ip: "", rssi: 0, battery: 50, batteryMv: 0,
  charging: false, heap: 0, uptime: 0, rev: 5, version: 2,
  reported: { ...settings.DEFAULT_SETTINGS, brightness: 70, project: "board-control-api", _project_api: 7 },
  pending: { volume: { value: 40, rev: 6 } }, projectSettings: { __board_control_api: { hash: "private-hash" } },
  settingsReported: true, commands: [{ id: "old-command", type: "identify", status: "queued", arg: "", result: "", createdAt: now, updatedAt: now }],
  tests: {}, samples: [], ota: null, networks: [],
};
let pairing = { mac: device.mac, code: "123456", secret: "board-secret", board: device.board,
  firmware: device.firmware, expiresAt: new Date(now + 60_000), deviceId: null };
let removed = 0;
const db = {
  device: {
    findFirst: async ({ where }) => device && Object.entries(where).every(([field, value]) => device[field] === value) ? structuredClone(device) : null,
    findUnique: async ({ where }) => device && (device.id === where.id || device.token === where.token) ? structuredClone(device) : null,
    findUniqueOrThrow: async () => structuredClone(device),
    findMany: async () => device ? [structuredClone(device)] : [],
    updateMany: async ({ where, data }) => {
      if (!device || device.id !== where.id || device.version !== where.version) return { count: 0 };
      device = { ...device, ...structuredClone(data), version: device.version + 1 };
      return { count: 1 };
    },
    update: async ({ where, data }) => {
      assert.equal(where.id, device.id);
      device = { ...device, ...structuredClone(data), version: device.version + 1 };
      return structuredClone(device);
    },
    deleteMany: async () => { removed++; device = null; return { count: 1 }; },
  },
  pairing: {
    findFirst: async ({ where }) => pairing && pairing.code === where.code && pairing.deviceId === null && pairing.expiresAt > where.expiresAt.gt ? structuredClone(pairing) : null,
    findMany: async () => pairing?.deviceId === null ? [{ mac: pairing.mac }] : [],
    updateMany: async ({ where, data }) => {
      if (!pairing || pairing.mac !== where.mac || pairing.code !== where.code || pairing.secret !== where.secret || pairing.deviceId !== where.deviceId) return { count: 0 };
      pairing = { ...pairing, ...data }; return { count: 1 };
    },
    update: async ({ where, data }) => { assert.equal(where.mac, pairing.mac); pairing = { ...pairing, ...data }; return pairing; },
  },
  $transaction: async (run) => run(db),
};
globalThis.__relinkTest = { db, DEFAULT_SETTINGS: settings.DEFAULT_SETTINGS };
let source = compile(readFileSync("src/lib/device-store.ts", "utf8"));
source = source.replace(/import .* from "@\/lib\/project-transfers";/, "const expireProjectCommands = (commands) => commands; const projectPending = () => false;")
  .replace(/import .* from "@\/lib\/db";/, "const { db } = globalThis.__relinkTest;")
  .replace(/import .* from "@\/lib\/board-control";/, 'const DEFAULT_MUSIC_URL = "https://navairan.com/;stream.nsv";')
  .replace(/import .* from "@\/lib\/device-settings";/, "const { DEFAULT_SETTINGS } = globalThis.__relinkTest;")
  .replace(/import .* from "@\/lib\/project-config";/, "const projectConfigsWithDefaults = (raw) => raw;");
const store = await import(`data:text/javascript;base64,${Buffer.from(source).toString("base64")}`);

assert.equal((await store.listDevices("owner"))[0].pairingAvailable, true);
await assert.rejects(() => store.claimCode("owner", "123456", "another-device"), /different board/);
assert.equal(pairing.deviceId, null, "A mismatched device must not consume the code");
const previousToken = device.token;
const linked = await store.claimCode("owner", "123456", "existing");
assert.equal(linked.id, "existing");
assert.equal(linked.name, "Living room");
assert.equal(linked.settings.brightness, 70);
assert.equal(linked.settings.volume, 40);
assert.notEqual(device.token, previousToken);
assert.equal(device.projectSettings.__board_control_api.hash, "private-hash");
assert.equal(device.commands[0].status, "failed", "Undelivered commands from the old pairing must not run");
assert.equal(device.pending.brightness.value, 70, "Saved settings must be sent to a reset board");
assert.equal(device.pending.volume.value, 40);
assert.equal(device.rev, 6);
assert.equal(pairing.deviceId, "existing");
assert.equal(removed, 0, "The existing device row and its history must survive");

const token = await store.rotateControlKey("owner", "existing");
assert.equal(token.length, 64);
const first = await store.updateControlDevice("existing", token, undefined, { type: "play", value: "" });
assert.equal(device.projectSettings.__board_control_api.requests[0].audio.value, "https://navairan.com/;stream.nsv");
const second = await store.updateControlDevice("existing", token, undefined, { type: "music", value: "https://example.com/song.mp3" });
await store.updateControlDevice("existing", token, undefined, { type: "pause", value: "" });
assert.equal(device.projectSettings.__board_control_api.requests[2].audio.type, "pause");
await store.updateControlDevice("existing", token, undefined, { type: "play", value: "" });
assert.equal(device.projectSettings.__board_control_api.requests[3].audio.type, "music");
assert.equal(device.projectSettings.__board_control_api.requests[3].audio.value, "https://example.com/song.mp3");
const beforeQueueRev = device.rev;
const settingsRequest = await store.updateControlDevice("existing", token, { brightness: 25 });
assert.equal(device.rev, beforeQueueRev, "Queued settings must not apply ahead of earlier audio actions");
assert.equal(settingsRequest.status, "queued");
assert.equal(await store.controlDevice("existing", "invalid"), null);

const report = {
  token: device.token, secret: "", code: "", mac: device.mac, board: device.board,
  firmware: device.firmware, ip: "", rssi: 0, battery: 50, batteryMv: 0, charging: false,
  heap: 0, uptime: 100, rev: device.rev, settings: device.reported, projectApi: 7,
  projectVersion: "1.0.3", projectSha256: "", projectSafeMode: false, sdCard: null,
  tests: {}, ota: null, networks: null, acks: [], projectStatus: null, projectLogs: [],
  fileProgress: null, resetReason: "", unlink: false,
};
const started = Date.now();
assert.equal((await store.syncBoard(report, 500, new AbortController().signal)).status, "linked");
assert.ok(Date.now() - started < 250, "A pending audio action should bypass the idle long poll");
assert.deepEqual(device.projectSettings.__board_control_api.requests.slice(0, 2).map((entry) => entry.status), ["sent", "queued"], "Only one control action may be sent at once");
assert.equal((await store.controlRequestForBoard(device.token)).id, first.id);
await store.syncBoard({ ...report, controlAck: { id: first.id, ok: true, result: "Music is playing" } }, 0, new AbortController().signal);
assert.deepEqual(device.projectSettings.__board_control_api.requests.slice(0, 2).map((entry) => entry.status), ["done", "sent"], "The next request starts only after the previous ACK");
assert.equal((await store.controlRequestStatus("existing", token, first.id)).result, "Music is playing");
assert.equal((await store.controlRequestForBoard(device.token)).id, second.id);
assert.equal((await store.controlRequestStatus("existing", token, settingsRequest.id)).status, "queued");
device.projectSettings.__board_control_api.requests[1].createdAt = Date.now() - 91_000;
assert.equal((await store.controlRequestStatus("existing", token, second.id)).status, "failed", "A missing board acknowledgment must time out");
await store.syncBoard({ ...report, controlAck: null }, 0, new AbortController().signal);
assert.equal(device.projectSettings.__board_control_api.requests[2].status, "sent", "A timeout must release the next queued action");

pairing = { ...pairing, code: "654321", deviceId: null, expiresAt: new Date(now - 1) };
assert.equal(await store.claimCode("owner", "654321", "existing"), null, "Expired codes cannot relink a board");
console.log("✓ Device relink and API token survive; control actions serialize, acknowledge and time out safely");
