import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, readdirSync, writeFileSync } from "node:fs";
import { homedir } from "node:os";
import { join } from "node:path";
const root = new URL("..", import.meta.url).pathname;
const tools = join(homedir(), "Library/Arduino15/packages/esp32/tools/esp-x32");
const versions = existsSync(tools) ? readdirSync(tools).sort().reverse() : [];
const gcc = process.env.PROJECT_CC || versions.map((v) => join(tools, v, "bin/xtensa-esp32s3-elf-gcc")).find(existsSync);
if (!gcc) throw new Error("Install the ESP32 Arduino toolchain or set PROJECT_CC to xtensa-esp32s3-elf-gcc.");
const strip = gcc.replace(/gcc$/, "strip");
const packages = [];
for (const dir of readdirSync(join(root, "projects"), {withFileTypes: true})) {
  if (!dir.isDirectory()) continue;
  const source = join(root, "projects", dir.name);
  if (!existsSync(join(source, "project.json"))) continue;
  const meta = JSON.parse(readFileSync(join(source, "project.json"), "utf8"));
  const destination = join(root, "public/projects/esp32-s3-lcd-0.85", meta.id);
  mkdirSync(destination, {recursive: true});
  const binary = join(destination, `${meta.version}.elf`);
  execFileSync(gcc, ["-Os", "-nostartfiles", "-nostdlib", "-fPIC", "-shared", "-Wl,-e,app_main", "-fdata-sections", "-ffunction-sections", "-Wl,--gc-sections", "-fvisibility=hidden", "-mtext-section-literals", "-mlongcalls", "-I", join(root, "firmware/esp32-s3-lcd-0.85/src/runtime"), join(source, "main.c"), "-o", binary], {stdio: "inherit"});
  execFileSync(strip, ["--strip-unneeded", "--remove-section=.comment", "--remove-section=.got.loc", "--remove-section=.dynamic", "--remove-section=.xt.lit", "--remove-section=.xt.prop", "--remove-section=.xtensa.info", binary]);
  const bytes = readFileSync(binary);
  packages.push({...meta, board: "esp32-s3-lcd-0.85", abi: 1, path: `/projects/esp32-s3-lcd-0.85/${meta.id}/${meta.version}.elf`, size: bytes.length, md5: createHash("md5").update(bytes).digest("hex"), sha256: createHash("sha256").update(bytes).digest("hex")});
  console.log(`✓ ${meta.id} v${meta.version} (${bytes.length} bytes)`);
}
writeFileSync(join(root, "projects/manifest.json"), JSON.stringify({projects: packages}, null, 2) + "\n");

execFileSync(process.execPath, [join(root, "scripts/verify-projects.mjs")], {stdio: "inherit"});
