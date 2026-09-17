import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import ts from "typescript";
const moduleFrom = async (path) => {
  const source = ts.transpileModule(readFileSync(path, "utf8"), { compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2022 } }).outputText;
  return import(`data:text/javascript;base64,${Buffer.from(source).toString("base64")}`);
};
const { inspectProject, inspectProjectUpload } = await moduleFrom("src/lib/project-file.ts");
let uploadReads = 0;
await assert.rejects(() => inspectProjectUpload({ size: 128 * 1024 + 1, arrayBuffer: async () => { uploadReads++; return new ArrayBuffer(0); } }), /128 KB/);
assert.equal(uploadReads, 0, "Oversized browser files must be rejected before reading");
await assert.rejects(() => inspectProjectUpload({ size: 0, arrayBuffer: async () => new ArrayBuffer(0) }), /between 1 byte/);
const manifest = JSON.parse(readFileSync("projects/manifest.json", "utf8"));
const deviceSettingsModule = await moduleFrom("src/lib/device-settings.ts");
globalThis.__projectConfigTest = { manifest, TIME_ZONES: deviceSettingsModule.TIME_ZONES };
let configSource = ts.transpileModule(readFileSync("src/lib/project-config.ts", "utf8"), { compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2022 } }).outputText;
configSource = configSource.replace(/import manifest from .*;/, "const { manifest } = globalThis.__projectConfigTest;").replace(/import .* from "@\/lib\/device-settings";/, "const { TIME_ZONES } = globalThis.__projectConfigTest;");
const projectConfig = await import(`data:text/javascript;base64,${Buffer.from(configSource).toString("base64")}`);
const weatherConfig = projectConfig.sanitizeProjectConfig("weather", { unit: "kelvin", details: false, location: { latitude: 1000, longitude: 13 } });
assert.equal(weatherConfig.unit, "celsius", "Invalid select values must fall back to the manifest default");
assert.equal(weatherConfig.details, false);
assert.deepEqual(weatherConfig.location, { latitude: 52.52, longitude: 13.405 }, "Invalid coordinates must not reach the weather service");
const clockConfig = projectConfig.sanitizeProjectConfig("analog-clock", { timezone: "Invalid/Zone", style: "minimal", seconds: "yes" });
assert.equal(clockConfig.timezone, "Europe/Berlin");
assert.equal(clockConfig.style, "minimal");
assert.equal(clockConfig.seconds, true);
const radioConfig = projectConfig.sanitizeProjectConfig("radio", { station: "bbc" });
assert.equal(radioConfig.station, "bbc");
assert.equal(projectConfig.sanitizeProjectConfig("radio", { station: "farda" }).station, "farda");
assert.equal(projectConfig.sanitizeProjectConfig("radio", { station: "missing" }).station, "farda");
const radioCustom = projectConfig.sanitizeProjectConfig("radio", { custom1: "  https://example.com/stream.mp3  ", custom2: "x".repeat(200) });
assert.equal(radioCustom.custom1, "https://example.com/stream.mp3", "Text settings must be trimmed");
assert.equal(radioCustom.custom2.length, 120, "Text settings must be capped at their manifest maxLength");
assert.equal(projectConfig.sanitizeProjectConfig("radio", { custom3: 5 }).custom3, "", "Non-string values must not overwrite a text setting's default");
const radioName = projectConfig.sanitizeProjectConfig("radio", { custom1_name: "Café Perse ۱۲۳", custom1_lang: "FA", custom2_lang: "invalid" });
assert.equal(radioName.custom1_name, "Caf Perse", "asciiOnly must strip non-ASCII characters (accents, Persian digits, ...)");
assert.equal(radioName.custom1_lang, "FA");
assert.equal(radioName.custom2_lang, "EN", "An unlisted select value must fall back to the manifest default");
assert.throws(() => projectConfig.sanitizeProjectConfig("missing", {}), /Unknown project/);
delete globalThis.__projectConfigTest;
for (const project of manifest.projects) {
  const file = readFileSync(`public${project.path}`);
  const identity = inspectProject(file);
  assert.equal((await inspectProjectUpload({ size: file.length, arrayBuffer: async () => Uint8Array.from(file).buffer })).id, project.id);
  assert.equal(identity.id, project.id);
  assert.equal(identity.version, project.version);
  assert.throws(() => inspectProject(file.subarray(0, 100)));
  const abiMarker = file.indexOf(Buffer.from(`"abi":${project.abi}`));
  assert.ok(abiMarker >= 0);
  for (const [abi, valid] of [[2, true], [3, true], [4, true], [5, true], [6, true], [7, false]]) {
    const changed = Buffer.from(file); changed[abiMarker + 6] = 48 + abi;
    if (valid) assert.equal(inspectProject(changed).abi, abi); else assert.throws(() => inspectProject(changed));
  }
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
const { DEFAULT_SETTINGS } = deviceSettingsModule;
let row = { id: "board", owner: "owner", token: "board-secret", name: "Test", mac: "00:00:00:00:00:00", board: "esp32-s3-lcd-0.85", firmware: "1.0.5", createdAt: new Date(), lastSeen: new Date(), reported: { ...DEFAULT_SETTINGS, _project_api: 1 }, pending: {}, version: 0, rev: 0, settingsReported: true, commands: [], samples: [], tests: {}, networks: [], ota: null, uptime: 10 };
const matches = (where) => Object.entries(where).every(([key, value]) => row[key] === value);
const storedFiles = new Map();
let nextFileId = 0;
const db = { device: {
  findUnique: async ({ where }) => matches(where) ? structuredClone(row) : null,
  findUniqueOrThrow: async () => structuredClone(row),
  findFirst: async ({ where }) => matches(where) ? structuredClone(row) : null,
  findMany: async () => [structuredClone(row)],
  updateMany: async ({ where, data }) => {
    if (!matches(where)) return { count: 0 };
    const version = row.version + 1; row = { ...row, ...structuredClone(data), version }; return { count: 1 };
  },
}, projectFile: {
  create: async ({ data }) => { const id = `file-${++nextFileId}`; storedFiles.set(id, { ...data, id }); return { id }; },
  findFirst: async ({ where }) => { const value = storedFiles.get(where.id); return value?.deviceId === where.deviceId ? value : null; },
  delete: async ({ where }) => { storedFiles.delete(where.id); },
  deleteMany: async ({ where }) => { let count = 0; for (const [id, file] of storedFiles) if (file.deviceId === where.deviceId && (!where.id?.in || where.id.in.includes(id)) && (!where.id?.notIn || !where.id.notIn.includes(id))) { storedFiles.delete(id); count++; } return { count }; },
} };
globalThis.__projectTest = { db, DEFAULT_SETTINGS, expireProjectCommands, projectPending: (c) => !!c && ["sent", "queued"].includes(c.status), projectConfigsWithDefaults: (raw) => raw ?? { weather: {}, "analog-clock": {} }, sanitizeProjectConfig: (_project, value) => value };
let storeSource = ts.transpileModule(readFileSync("src/lib/device-store.ts", "utf8"), { compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2022 } }).outputText;
storeSource = storeSource.replace(/import .* from "@\/lib\/project-transfers";/, "const { expireProjectCommands, projectPending } = globalThis.__projectTest;").replace(/import .* from "@\/lib\/db";/, "const { db } = globalThis.__projectTest;").replace(/import .* from "@\/lib\/device-settings";/, "const { DEFAULT_SETTINGS } = globalThis.__projectTest;").replace(/import .* from "@\/lib\/project-config";/, "const { projectConfigsWithDefaults } = globalThis.__projectTest;");
const store = await import(`data:text/javascript;base64,${Buffer.from(storeSource).toString("base64")}`);
row.firmware = "1.1.25";
row.reported._project_api = 5;
assert.equal((await store.listDevices("owner"))[0].projectApi, 6, "Firmware 1.1.25 must expose its compiled ABI 6 despite the stale sync value");
row.firmware = "1.1.24";
assert.equal((await store.listDevices("owner"))[0].projectApi, 5, "Older firmware must keep its reported ABI");
row.firmware = "1.0.5";
row.reported._project_api = 1;
const file = readFileSync(`public${manifest.projects[0].path}`);
const meta = { name: "Local clock", version: "1.0.1", size: file.length, bytes: file };
const queued = await store.queueCommand("owner", "board", "project_install", "id=custom-clock&path=%2Fapi%2Fdevices%2F&abi=1", "", meta);
assert.equal(queued.commands[0].fileId, undefined, "Uploaded file references must never appear in public responses");
const first = row.commands[0];
assert.ok(first.fileId && storedFiles.has(first.fileId));
assert.ok(new URLSearchParams(first.arg).get("path").includes(first.id));
assert.equal(await store.projectFileForBoard("board", first.id, "wrong-token"), null);
assert.deepEqual(await store.projectFileForBoard("board", first.id, "board-secret"), file);
await store.stopProject("owner", "board", first.id);
assert.equal(row.commands.length, 1, "Cancelling an undelivered request must not contact the board");
assert.equal(row.commands[0].transfer.phase, "cancelled");
await store.retryProjectFile("owner", "board", first.id);
const retry = row.commands.at(-1);
assert.notEqual(retry.id, first.id); assert.equal(retry.fileId, first.fileId);
assert.ok(new URLSearchParams(retry.arg).get("path").includes(retry.id));
const report = { token: "board-secret", secret: "", mac: row.mac, board: row.board, firmware: "1.0.5", uptime: 11, rev: 0, settings: DEFAULT_SETTINGS, projectApi: 2, projectVersion: "1.1.0", projectSha256: "a".repeat(64), projectSafeMode: false, tests: {}, acks: [], projectStatus: null, projectLogs: [], networks: null, ota: null, resetReason: "", unlink: false };
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
await store.syncBoard({ ...report, uptime: 13, projectApi: 3, sdCard: { mounted: true, total: 32_000_000_000, free: 1_000_000 } }, 0, new AbortController().signal);
assert.deepEqual((await store.listDevices("owner"))[0].sdCard, { mounted: true, total: 32_000_000_000, free: 1_000_000 }, "Reported SD card state must reach the console");
await store.syncBoard({ ...report, uptime: 13, projectApi: 3 }, 0, new AbortController().signal);
assert.equal((await store.listDevices("owner"))[0].sdCard, null, "Firmware without SD reporting must not show a stale card");


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
routeSource = routeSource.replace(/import .* from "@\/lib\/auth";/, "const { getUser, unauthorized } = globalThis.__projectTest;").replace(/import .* from "@\/lib\/device-settings";/, "const { DEFAULT_SETTINGS, sanitizeSettings } = globalThis.__projectTest;").replace(/import .* from "@\/lib\/device-store";/, "const { listDevices, removeDevice, updateDevice } = globalThis.__projectTest;").replace(/import .* from "@\/lib\/project-config";/, "const { sanitizeProjectConfig } = globalThis.__projectTest;");
const route = await import(`data:text/javascript;base64,${Buffer.from(routeSource).toString("base64")}`);
const patch = (settings) => route.PATCH(new Request("http://test/api/devices/board", { method: "PATCH", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ settings }) }), { params: Promise.resolve({ id: "board" }) });
assert.equal((await patch({ ...DEFAULT_SETTINGS, project: "weather", brightness: 50 })).status, 200, "Full Configure settings with unchanged project must save");
assert.equal(row.pending.brightness.value, 50);
assert.equal((await patch({ project: "analog-clock", brightness: 60 })).status, 400, "Configure must not switch native modules");
assert.equal(row.pending.brightness.value, 50);
console.log("✓ Duplicate crash logs, history cleanup and Configure regression checks passed");
// SD card file manager: queue, board transfer, listing parse, upload delivery and serialization.
const deviceFiles = new Map();
db.deviceFile = {
  deleteMany: async ({ where }) => { for (const [id, file] of deviceFiles) if (file.deviceId === where.deviceId && file.createdAt < where.createdAt.lt) deviceFiles.delete(id); return { count: 0 }; },
  create: async ({ data }) => { deviceFiles.set(data.id, { ...data, createdAt: new Date() }); return data; },
  delete: async ({ where }) => { deviceFiles.delete(where.id); },
  findFirst: async ({ where }) => { const file = deviceFiles.get(where.id); return file?.deviceId === where.deviceId ? file : null; },
  upsert: async ({ where, create }) => { deviceFiles.set(where.id, { ...create, createdAt: new Date() }); return create; },
};
await assert.rejects(() => store.queueFileCommand("owner", "board", "sd_list", { path: "/" }), /project installation/);
row.commands = row.commands.filter((c) => c.type !== "project_install");
assert.equal(await store.queueFileCommand("other-owner", "board", "sd_list", { path: "/" }), null);
const listing = await store.queueFileCommand("owner", "board", "sd_list", { path: "/" });
assert.equal(new URLSearchParams(row.commands.at(-1).arg).get("src"), `/api/device/files/${listing.job}`);
await assert.rejects(() => store.queueFileCommand("owner", "board", "sd_mkdir", { path: "/x" }), /Another SD card operation/);
assert.equal(await store.storeBoardTransfer("board-secret", listing.job, Buffer.from("d\t0\t0\tclips\n")), false, "Undelivered operations must not accept board data");
await store.syncBoard({ ...report, uptime: 20 }, 0, new AbortController().signal);
assert.equal(row.commands.find((c) => c.id === listing.job).status, "sent");
assert.equal(await store.storeBoardTransfer("wrong-token", listing.job, Buffer.from("")), false);
assert.ok(await store.storeBoardTransfer("board-secret", listing.job, Buffer.from("f\t12\t1700000000\tb.txt\nd\t0\t0\tclips\nf\t3\t0\ta\ttab.bin\n")));
assert.equal((await store.fileJob("owner", "board", listing.job)).job.status, "sent");
await store.syncBoard({ ...report, uptime: 21, acks: [{ id: listing.job, ok: true, result: "3 entries" }] }, 0, new AbortController().signal);
const listed = await store.fileJob("owner", "board", listing.job);
assert.deepEqual(listed.job.entries.map((e) => [e.name, e.folder, e.size]), [["clips", true, 0], ["a\ttab.bin", false, 3], ["b.txt", false, 12]], "Folders first, names may contain tabs");
assert.equal(await store.fileJob("other-owner", "board", listing.job), null);
const upload = await store.queueFileCommand("owner", "board", "sd_upload", { path: "/clips/a.bin" }, Buffer.from("hello"));
const uploadArg = new URLSearchParams(row.commands.at(-1).arg);
assert.equal(uploadArg.get("size"), "5");
assert.equal(uploadArg.get("sha256"), "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
assert.equal(await store.uploadForBoard("board-secret", upload.job), null, "Upload bytes are only served once delivered");
await store.syncBoard({ ...report, uptime: 22 }, 0, new AbortController().signal);
assert.equal((await store.uploadForBoard("board-secret", upload.job)).toString(), "hello");
assert.equal(await store.uploadForBoard("wrong-token", upload.job), null);
console.log("✓ SD card file manager queueing, board transfers and listings checks passed");
row.ota = { state: "queued", progress: 0, error: "", version: "1.1.22", updatedAt: Date.now() };
row.commands.push({ id: "ota-fail", type: "ota", arg: "", status: "failed", result: "No memory for update task", createdAt: Date.now(), updatedAt: Date.now() });
assert.deepEqual((await store.listDevices("owner"))[0].ota, {
  state: "failed", progress: 0, error: "No memory for update task", version: "1.1.22", updatedAt: row.commands.at(-1).updatedAt,
}, "A failed OTA command must not leave the panel waiting for the device");
delete globalThis.__projectTest;
