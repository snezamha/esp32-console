#!/usr/bin/env node
// Compiles every Arduino sketch in firmware/ and stores the flashable image in
// public/firmware/<board>/<version>.bin (merged, USB flashing at 0x0) and the application image in
// <version>.app.bin (over-the-air updates), then updates firmware/manifest.json.
// `preserve` regions in board.json (e.g. the NVS settings partition) are skipped
// by the web flasher so user settings survive updates.
//
// Set CONSOLE_URL (e.g. https://console.example.com) to build in the web console address that
// boards check in with; boards can also be pointed at a console later over USB.
//
// Usage: [CONSOLE_URL=…] pnpm firmware:build [board-id] [--force]

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const root = new URL("..", import.meta.url).pathname;
const firmwareDir = join(root, "firmware");
const publicDir = join(root, "public", "firmware");
const manifestPath = join(firmwareDir, "manifest.json");

const args = process.argv.slice(2);
const force = args.includes("--force");
const only = args.find((a) => !a.startsWith("--"));
const consoleUrl = (process.env.CONSOLE_URL ?? "").trim().replace(/\/+$/, "");
if (consoleUrl && !/^https?:\/\/[^\s"\\]+$/.test(consoleUrl)) {
  console.error(`CONSOLE_URL must be an http(s) URL without spaces or quotes: ${consoleUrl}`);
  process.exit(1);
}

const manifest = existsSync(manifestPath)
  ? JSON.parse(readFileSync(manifestPath, "utf8"))
  : { boards: [] };

const boardDirs = readdirSync(firmwareDir, { withFileTypes: true })
  .filter((d) => d.isDirectory() && existsSync(join(firmwareDir, d.name, "board.json")))
  .map((d) => d.name)
  .filter((name) => !only || name === only);

if (boardDirs.length === 0) {
  console.error(only ? `Board "${only}" not found in firmware/` : "No boards found in firmware/");
  process.exit(1);
}

for (const dir of boardDirs) {
  const sketchDir = join(firmwareDir, dir);
  const board = JSON.parse(readFileSync(join(sketchDir, "board.json"), "utf8"));
  const versionHeader = readFileSync(join(sketchDir, "version.h"), "utf8");
  const version = /#define\s+FIRMWARE_VERSION\s+"([^"]+)"/.exec(versionHeader)?.[1];
  if (!version) throw new Error(`${dir}/version.h: FIRMWARE_VERSION not found`);

  const outFile = join(publicDir, board.id, `${version}.bin`);
  if (existsSync(outFile) && !force) {
    console.error(`${board.id} v${version} already exists. Bump version.h or pass --force.`);
    process.exit(1);
  }

  console.log(`▸ Building ${board.id} v${version}`);
  const buildDir = mkdtempSync(join(tmpdir(), "esp32-console-"));
  try {
    // Build-time console address, picked up by src/services/console_client.cpp. Passing it as a
    // -D flag through arduino-cli mangles the quotes, so it goes through a generated header.
    const urlHeader = join(sketchDir, "console_url.h");
    rmSync(urlHeader, { force: true });
    if (consoleUrl) writeFileSync(urlHeader, `#pragma once\n#define CONSOLE_URL ${JSON.stringify(consoleUrl)}\n`);
    try {
      execFileSync(
        "arduino-cli",
        ["compile", "--fqbn", board.fqbn, "--build-path", buildDir, sketchDir],
        { stdio: "inherit" },
      );
    } finally {
      rmSync(urlHeader, { force: true });
    }

    const merged = readFileSync(join(buildDir, `${dir}.ino.merged.bin`));
    const image = trimErasedTail(merged);

    const app = readFileSync(join(buildDir, `${dir}.ino.bin`));

    mkdirSync(join(publicDir, board.id), { recursive: true });
    writeFileSync(outFile, image);
    writeFileSync(join(publicDir, board.id, `${version}.app.bin`), app);

    const meta = { ...board };
    delete meta.fqbn;
    const index = manifest.boards.findIndex((b) => b.id === board.id);
    const previous = index >= 0 ? manifest.boards[index].versions : [];
    const versions = previous
      .filter((v) => v.version !== version)
      .concat({
        version,
        builtAt: new Date().toISOString(),
        size: image.length,
        sha256: createHash("sha256").update(image).digest("hex"),
        app: {
          size: app.length,
          sha256: createHash("sha256").update(app).digest("hex"),
          md5: createHash("md5").update(app).digest("hex"),
        },
      })
      .sort((a, b) => compareVersions(b.version, a.version));
    const entry = { ...meta, versions };
    if (index >= 0) manifest.boards[index] = entry;
    else manifest.boards.push(entry);

    console.log(`✓ public/firmware/${board.id}/${version}.bin (${(image.length / 1024).toFixed(0)} KB)`);
  } finally {
    rmSync(buildDir, { recursive: true, force: true });
  }
}

writeFileSync(manifestPath, JSON.stringify(manifest, null, 2) + "\n");
console.log("✓ firmware/manifest.json updated");

/** The merged image is padded with 0xFF to the full flash size; drop the padding. */
function trimErasedTail(buffer) {
  let end = buffer.length;
  while (end > 0 && buffer[end - 1] === 0xff) end--;
  const aligned = Math.ceil(end / 4096) * 4096;
  return buffer.subarray(0, Math.min(aligned, buffer.length));
}

function compareVersions(a, b) {
  const pa = a.split(/[.-]/).map((n) => parseInt(n, 10) || 0);
  const pb = b.split(/[.-]/).map((n) => parseInt(n, 10) || 0);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    if ((pa[i] ?? 0) !== (pb[i] ?? 0)) return (pa[i] ?? 0) - (pb[i] ?? 0);
  }
  return 0;
}
