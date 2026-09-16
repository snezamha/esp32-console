import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { copyFileSync, existsSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, unlinkSync, writeFileSync } from "node:fs";
import { homedir } from "node:os";
import { dirname, extname, join, relative, sep } from "node:path";
const root = new URL("..", import.meta.url).pathname;
const tools = join(homedir(), "Library/Arduino15/packages/esp32/tools/esp-x32");
const versions = existsSync(tools) ? readdirSync(tools).sort().reverse() : [];
const gcc = process.env.PROJECT_CC || versions.map((v) => join(tools, v, "bin/xtensa-esp32s3-elf-gcc")).find(existsSync);
if (!gcc) throw new Error("Install the ESP32 Arduino toolchain or set PROJECT_CC to xtensa-esp32s3-elf-gcc.");
const strip = gcc.replace(/gcc$/, "strip");
const objcopy = gcc.replace(/gcc$/, "objcopy");
const board = "esp32-s3-lcd-0.85";
// Files in projects/<id>/assets are installed onto the board's SD card. Images and videos are
// converted to the firmware's raw RGB565 media format; every other file is copied unchanged.
const IMAGE = new Set([".png", ".jpg", ".jpeg", ".webp", ".bmp"]);
const VIDEO = new Set([".mp4", ".mov", ".webm", ".mkv", ".avi", ".gif"]);
const ASSET_SEGMENT = /^[A-Za-z0-9_-][A-Za-z0-9._-]*$/;
const packages = [];
for (const dir of readdirSync(join(root, "projects"), {withFileTypes: true})) {
  if (!dir.isDirectory()) continue;
  const source = join(root, "projects", dir.name);
  if (!existsSync(join(source, "project.json"))) continue;
  const meta = JSON.parse(readFileSync(join(source, "project.json"), "utf8"));
  const assetSource = join(source, "assets");
  const hasAssets = existsSync(assetSource);
  const abi = meta.abi ?? (hasAssets ? 3 : 2);
  if (!Number.isInteger(abi) || abi < 2 || abi > 5 || (hasAssets && abi < 3)) throw new Error(`${meta.id}: unsupported project ABI ${abi}.`);
  const destination = join(root, "public/projects", board, meta.id);
  mkdirSync(destination, {recursive: true});
  const binary = join(destination, `${meta.version}.elf`);
  execFileSync(gcc, ["-Os", "-nostartfiles", "-nostdlib", "-fPIC", "-shared", "-Wl,-e,app_main", "-fdata-sections", "-ffunction-sections", "-Wl,--gc-sections", "-fvisibility=hidden", "-mtext-section-literals", "-mlongcalls", `-DDISPLAY_PROJECT_ABI=${abi}`, "-I", join(root, "firmware", board, "src/runtime"), join(source, "main.c"), "-o", binary], {stdio: "inherit"});
  execFileSync(strip, ["--strip-unneeded", "--remove-section=.comment", "--remove-section=.got.loc", "--remove-section=.dynamic", "--remove-section=.xt.lit", "--remove-section=.xt.prop", "--remove-section=.xtensa.info", binary]);
  const metadata = join(destination, ".metadata.json");
  writeFileSync(metadata, JSON.stringify({...meta, board, abi}));
  execFileSync(objcopy, ["--add-section", `.project=${metadata}`, binary]);
  unlinkSync(metadata);
  const bytes = readFileSync(binary);
  const entry = {...meta, board, abi, path: `/projects/${board}/${meta.id}/${meta.version}.elf`, size: bytes.length, md5: hash("md5", bytes), sha256: hash("sha256", bytes)};
  if (hasAssets) Object.assign(entry, buildAssets(meta, assetSource, join(destination, meta.version)));
  packages.push(entry);
  console.log(`✓ ${meta.id} v${meta.version} (${bytes.length} bytes${entry.storage ? `, ${entry.storage.files} SD card files, ${(entry.storage.bytes / 1048576).toFixed(1)} MB` : ""})`);
}
writeFileSync(join(root, "projects/manifest.json"), JSON.stringify({projects: packages}, null, 2) + "\n");

execFileSync(process.execPath, [join(root, "scripts/verify-projects.mjs")], {stdio: "inherit"});

function hash(algorithm, bytes) {
  return createHash(algorithm).update(bytes).digest("hex");
}

function buildAssets(meta, sourceDir, versionDir) {
  const media = { maxWidth: 128, maxHeight: 128, fps: 15, ...meta.media };
  const outputDir = join(versionDir, "assets");
  rmSync(versionDir, {recursive: true, force: true});
  mkdirSync(outputDir, {recursive: true});
  const lines = [];
  let total = 0;
  for (const file of walk(sourceDir)) {
    const name = relative(sourceDir, file).split(sep).join("/");
    const extension = extname(name).toLowerCase();
    const kind = IMAGE.has(extension) ? "img" : VIDEO.has(extension) ? "vid" : null;
    const outputName = kind ? name.slice(0, -extension.length) + "." + kind : name;
    const segments = outputName.split("/");
    if (outputName.length > 96 || segments.length > 4 || !segments.every((segment) => ASSET_SEGMENT.test(segment))) {
      throw new Error(`${meta.id}: asset name "${outputName}" must be at most 4 folders deep, 96 characters, and use only letters, digits, ".", "_" and "-".`);
    }
    if (lines.some((line) => line.startsWith(outputName + "|"))) throw new Error(`${meta.id}: two assets convert to "${outputName}".`);
    const output = join(outputDir, ...segments);
    mkdirSync(dirname(output), {recursive: true});
    if (kind) writeFileSync(output, convertMedia(file, kind, media));
    else copyFileSync(file, output);
    const bytes = readFileSync(output);
    lines.push(`${outputName}|${bytes.length}|${hash("sha256", bytes)}`);
    total += bytes.length;
  }
  if (!lines.length) throw new Error(`${meta.id}: assets folder is empty.`);
  if (lines.length > 2000) throw new Error(`${meta.id}: at most 2000 SD card files are supported.`);
  const index = Buffer.from(lines.join("\n") + "\n");
  if (index.length > 64 * 1024) throw new Error(`${meta.id}: SD card file list exceeds 64 KB.`);
  writeFileSync(join(versionDir, "assets.txt"), index);
  return {
    storage: { sd: true, bytes: total, files: lines.length },
    assets: { path: `/projects/${board}/${meta.id}/${meta.version}/assets.txt`, size: index.length, sha256: hash("sha256", index) },
  };
}

function* walk(dir) {
  for (const entry of readdirSync(dir, {withFileTypes: true}).sort((a, b) => a.name.localeCompare(b.name))) {
    if (entry.name.startsWith(".")) continue;
    const path = join(dir, entry.name);
    if (entry.isDirectory()) yield* walk(path);
    else if (entry.isFile() && statSync(path).size > 0) yield path;
  }
}

// Header (little-endian): "EVM1", width, height, frames, fps as uint16, then 4 reserved bytes,
// followed by frames of big-endian RGB565 pixels, row by row. Transparency is flattened to black.
function convertMedia(file, kind, media) {
  const probe = JSON.parse(execFileSync("ffprobe", ["-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height", "-of", "json", file], {encoding: "utf8"}));
  const stream = probe.streams?.[0];
  if (!stream?.width || !stream?.height) throw new Error(`${file}: not a readable image or video.`);
  const scale = Math.min(1, media.maxWidth / stream.width, media.maxHeight / stream.height);
  const width = Math.max(1, Math.round(stream.width * scale)), height = Math.max(1, Math.round(stream.height * scale));
  const filters = [`scale=${width}:${height}:flags=lanczos`];
  if (kind === "vid") filters.unshift(`fps=${media.fps}`);
  const pixels = execFileSync("ffmpeg", ["-v", "error", "-i", file, "-vf", filters.join(","), ...(kind === "img" ? ["-frames:v", "1"] : []), "-f", "rawvideo", "-pix_fmt", "rgb565be", "pipe:1"], {maxBuffer: 4 * 1024 * 1024 * 1024});
  const frames = pixels.length / (width * height * 2);
  if (!Number.isInteger(frames) || frames < 1) throw new Error(`${file}: conversion produced no complete frames.`);
  if (frames > 65535) throw new Error(`${file}: more than 65535 frames; shorten it or lower media.fps.`);
  const header = Buffer.alloc(16);
  header.write("EVM1", 0, "ascii");
  header.writeUInt16LE(width, 4); header.writeUInt16LE(height, 6); header.writeUInt16LE(frames, 8); header.writeUInt16LE(kind === "vid" ? media.fps : 0, 10);
  return Buffer.concat([header, pixels]);
}
