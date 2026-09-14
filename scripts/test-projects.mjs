import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import ts from "typescript";
const moduleFrom = async (path) => {
  const source = ts.transpileModule(readFileSync(path, "utf8"), { compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2022 } }).outputText;
  return import(`data:text/javascript;base64,${Buffer.from(source).toString("base64")}`);
};
const { inspectProject } = await moduleFrom("src/lib/project-file.ts");
const manifest = JSON.parse(readFileSync("projects/manifest.json", "utf8"));
for (const project of manifest.projects) {
  const file = readFileSync(`public${project.path}`);
  const identity = inspectProject(file);
  assert.equal(identity.id, project.id);
  assert.equal(identity.version, project.version);
  assert.throws(() => inspectProject(file.subarray(0, 100)));
  const wrongCpu = Buffer.from(file); wrongCpu.writeUInt16LE(243, 18);
  assert.throws(() => inspectProject(wrongCpu));
  const noMetadata = Buffer.from(file);
  const marker = noMetadata.indexOf(Buffer.from(".project\0"));
  assert.ok(marker >= 0); noMetadata[marker + 1] = 120;
  assert.throws(() => inspectProject(noMetadata));
  const corruptNames = Buffer.from(file);
  corruptNames.writeUInt32LE(0xffffffff, corruptNames.readUInt32LE(32) + 40);
  assert.throws(() => inspectProject(corruptNames));
}
const { expireProjectCommands } = await moduleFrom("src/lib/project-transfers.ts");
const now = 1_000_000;
const base = { id: "test", type: "project_install", arg: "", result: "", createdAt: 1, updatedAt: 1, status: "queued", transfer: { phase: "queued", progress: 0, bytes: 0, total: 100, name: "Test", version: "1.0.0", logs: [] } };
assert.equal(expireProjectCommands([base], now)[0].status, "failed");
assert.equal(expireProjectCommands([{ ...base, updatedAt: now - 599_999 }], now)[0].status, "queued");
assert.equal(expireProjectCommands([{ ...base, status: "sent", updatedAt: now - 120_000 }], now)[0].status, "failed");
const cancelled = { ...base, status: "failed", transfer: { ...base.transfer, phase: "cancelled" } };
assert.equal(expireProjectCommands([cancelled], now)[0], cancelled);
assert.equal(base.status, "queued", "Expiration must not mutate persisted rows");
console.log("✓ Standalone file identity, invalid files, offline expiration and cancellation checks passed");
// Exercise actual persistence/delivery code with an in-memory Prisma device delegate.
const { DEFAULT_SETTINGS } = await moduleFrom("src/lib/device-settings.ts");
let row = { id: "board", owner: "owner", token: "board-secret", name: "Test", mac: "00:00:00:00:00:00", board: "esp32-s3-lcd-0.85", firmware: "1.0.5", createdAt: new Date(), lastSeen: new Date(), reported: { ...DEFAULT_SETTINGS, _project_api: 1 }, pending: {}, version: 0, rev: 0, settingsReported: true, commands: [], samples: [], tests: {}, networks: [], ota: null, uptime: 10 };
const matches = (where) => Object.entries(where).every(([key, value]) => row[key] === value);
const db = { device: {
  findUnique: async ({ where }) => matches(where) ? structuredClone(row) : null,
  findUniqueOrThrow: async () => structuredClone(row),
  findFirst: async ({ where }) => matches(where) ? structuredClone(row) : null,
  findMany: async () => [structuredClone(row)],
  updateMany: async ({ where, data }) => {
    if (!matches(where)) return { count: 0 };
    const version = row.version + 1; row = { ...row, ...structuredClone(data), version }; return { count: 1 };
  },
} };
globalThis.__projectTest = { db, DEFAULT_SETTINGS, expireProjectCommands, projectPending: (c) => !!c && ["sent", "queued"].includes(c.status) };
let storeSource = ts.transpileModule(readFileSync("src/lib/device-store.ts", "utf8"), { compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2022 } }).outputText;
storeSource = storeSource.replace(/import .* from "@\/lib\/project-transfers";/, "const { expireProjectCommands, projectPending } = globalThis.__projectTest;").replace(/import .* from "@\/lib\/db";/, "const { db } = globalThis.__projectTest;").replace(/import .* from "@\/lib\/device-settings";/, "const { DEFAULT_SETTINGS } = globalThis.__projectTest;");
const store = await import(`data:text/javascript;base64,${Buffer.from(storeSource).toString("base64")}`);
const file = readFileSync(`public${manifest.projects[0].path}`).toString("base64");
const meta = { name: "Local clock", version: "1.0.1", size: Buffer.from(file, "base64").length, file };
const queued = await store.queueCommand("owner", "board", "project_install", "id=custom-clock&path=%2Fapi%2Fdevices%2F&abi=1", "", meta);
assert.equal(queued.commands[0].file, undefined, "Uploaded bytes must never appear in public responses");
const first = row.commands[0];
assert.ok(new URLSearchParams(first.arg).get("path").includes(first.id));
assert.equal(await store.projectFileForBoard("board", first.id, "wrong-token"), null);
assert.equal((await store.projectFileForBoard("board", first.id, "board-secret")).toString("base64"), file);
await store.stopProject("owner", "board", first.id);
assert.equal(row.commands.length, 1, "Cancelling an undelivered request must not contact the board");
assert.equal(row.commands[0].transfer.phase, "cancelled");
await store.retryProjectFile("owner", "board", first.id);
const retry = row.commands.at(-1);
assert.notEqual(retry.id, first.id); assert.equal(retry.file, file);
assert.ok(new URLSearchParams(retry.arg).get("path").includes(retry.id));
const report = { token: "board-secret", secret: "", mac: row.mac, board: row.board, firmware: "1.0.5", uptime: 11, rev: 0, settings: DEFAULT_SETTINGS, projectSupported: true, tests: {}, acks: [], projectStatus: null, projectLogs: [], networks: null, ota: null, resetReason: "", unlink: false };
await store.syncBoard(report, 0, new AbortController().signal);
assert.equal(row.commands.at(-1).status, "sent");
await store.stopProject("owner", "board", retry.id);
assert.equal(row.commands.at(-1).type, "project_stop");
await assert.rejects(() => store.retryProjectFile("owner", "board", retry.id), /already running/);
await store.syncBoard({ ...report, uptime: 12 }, 0, new AbortController().signal);
const stop = row.commands.at(-1);
await store.syncBoard({ ...report, uptime: 13, acks: [{ id: stop.id, ok: true, result: "Transfer stopped" }] }, 0, new AbortController().signal);
assert.equal(row.commands.at(-1).status, "done");
assert.ok(row.commands.find((c) => c.id === retry.id).transfer.logs.some((log) => log.message === "Transfer stopped"));
await store.retryProjectFile("owner", "board", retry.id);
console.log("✓ Private upload delivery, queued/sent stop, confirmation and file retry checks passed");


const running = row.commands.at(-1);
await store.syncBoard({ ...report, uptime: 14 }, 0, new AbortController().signal);
const crash = "Board restarted during project installation (reset 4). Previous project restored.";
const crashReport = { ...report, uptime: 15, projectLogs: [{ id: running.id, seq: 1, level: "error", message: crash }], acks: [{ id: running.id, ok: false, result: crash }] };
await store.syncBoard(crashReport, 0, new AbortController().signal);
assert.equal(row.commands.find((c) => c.id === running.id).transfer.logs.filter((l) => l.message === crash).length, 1);
await store.syncBoard({ ...crashReport, uptime: 16 }, 0, new AbortController().signal);
assert.equal(row.commands.find((c) => c.id === running.id).transfer.logs.filter((l) => l.message === crash).length, 1, "Repeated ACKs must not duplicate error logs");
await store.queueCommand("owner", "board", "project_install", "id=none");
row.reported.project = "weather";
row.commands.push({ ...base, id: "ordinary-command", type: "identify", status: "done", transfer: undefined });
const count = row.commands.length;
assert.equal(await store.clearProjectHistory("other-owner", "board"), null);
assert.equal(row.commands.length, count);
const cleared = await store.clearProjectHistory("owner", "board");
assert.equal(cleared.activeProject, "weather", "Clearing history must not unload a project");
assert.equal(row.commands.filter((c) => c.type === "project_install").length, 1);
assert.equal(row.commands.find((c) => c.type === "project_install").status, "queued");
assert.ok(row.commands.some((c) => c.id === "ordinary-command"));
const { sanitizeSettings } = await moduleFrom("src/lib/device-settings.ts");
Object.assign(globalThis.__projectTest, { getUser: async () => ({ id: "owner" }), unauthorized: () => new Response(null, { status: 401 }), sanitizeSettings, listDevices: store.listDevices, removeDevice: store.removeDevice, updateDevice: store.updateDevice });
let routeSource = ts.transpileModule(readFileSync("src/app/api/devices/[id]/route.ts", "utf8"), { compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2022 } }).outputText;
routeSource = routeSource.replace(/import .* from "@\/lib\/auth";/, "const { getUser, unauthorized } = globalThis.__projectTest;").replace(/import .* from "@\/lib\/device-settings";/, "const { DEFAULT_SETTINGS, sanitizeSettings } = globalThis.__projectTest;").replace(/import .* from "@\/lib\/device-store";/, "const { listDevices, removeDevice, updateDevice } = globalThis.__projectTest;");
const route = await import(`data:text/javascript;base64,${Buffer.from(routeSource).toString("base64")}`);
const patch = (settings) => route.PATCH(new Request("http://test/api/devices/board", { method: "PATCH", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ settings }) }), { params: Promise.resolve({ id: "board" }) });
assert.equal((await patch({ ...DEFAULT_SETTINGS, project: "weather", brightness: 50 })).status, 200, "Full Configure settings with unchanged project must save");
assert.equal(row.pending.brightness.value, 50);
assert.equal((await patch({ project: "analog-clock", brightness: 60 })).status, 400, "Configure must not switch native modules");
assert.equal(row.pending.brightness.value, 50);
console.log("✓ Duplicate crash logs, history cleanup and Configure regression checks passed");
delete globalThis.__projectTest;
