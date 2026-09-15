import "server-only";

import { execFile } from "node:child_process";
import { mkdtemp, readFile, readdir, rm, writeFile } from "node:fs/promises";
import { existsSync } from "node:fs";
import { homedir, tmpdir } from "node:os";
import { join } from "node:path";
import { promisify } from "node:util";
import { inspectProject, MAX_PROJECT_FILE_SIZE, type ProjectMetadata } from "@/lib/project-file";

const runFile = promisify(execFile);
const BOARD = "esp32-s3-lcd-0.85";
const ABI = 4;
const MAX_SOURCE_SIZE = 48 * 1024;
const ID = /^[a-z0-9][a-z0-9-]{0,47}$/;
const VERSION = /^\d+\.\d+\.\d+$/;

export type BuilderInput = Pick<ProjectMetadata, "id" | "name" | "version" | "description"> & { source: string };

export async function compileProject(input: BuilderInput) {
  validate(input);
  const compiler = await findCompiler();
  if (!compiler) throw new BuilderError("The ESP32-S3 compiler is not installed on this server.", 503);
  const strip = compiler.replace(/gcc$/, "strip");
  const objcopy = compiler.replace(/gcc$/, "objcopy");
  if (!existsSync(strip) || !existsSync(objcopy)) throw new BuilderError("The ESP32-S3 compiler tools are incomplete.", 503);

  const root = process.cwd();
  const includeDir = join(root, "firmware", BOARD, "src", "runtime");
  const directory = await mkdtemp(join(tmpdir(), "esp32-project-builder-"));
  const sourcePath = join(directory, "sketch.ino");
  const binaryPath = join(directory, `${input.id}.elf`);
  const metadataPath = join(directory, "project.json");
  try {
    await writeFile(sourcePath, input.source, "utf8");
    await command(compiler, [
      "-x", "c", "-std=gnu11", "-Os", "-nostartfiles", "-nostdlib", "-ffreestanding", "-fno-builtin",
      "-fPIC", "-shared", "-Wl,-e,app_main", "-fdata-sections", "-ffunction-sections", "-Wl,--gc-sections",
      "-fvisibility=hidden", "-mtext-section-literals", "-mlongcalls", "-Wall", "-Werror=implicit-function-declaration",
      `-DDISPLAY_PROJECT_ABI=${ABI}`, "-I", includeDir, sourcePath, "-o", binaryPath,
    ]);
    await command(strip, [
      "--strip-unneeded", "--remove-section=.comment", "--remove-section=.got.loc", "--remove-section=.dynamic",
      "--remove-section=.xt.lit", "--remove-section=.xt.prop", "--remove-section=.xtensa.info", binaryPath,
    ]);
    await writeFile(metadataPath, JSON.stringify({
      id: input.id,
      name: input.name.trim(),
      version: input.version,
      description: input.description.trim().slice(0, 300),
      board: BOARD,
      abi: ABI,
    }));
    await command(objcopy, ["--add-section", `.project=${metadataPath}`, binaryPath]);
    const bytes = await readFile(binaryPath);
    if (bytes.length > MAX_PROJECT_FILE_SIZE) throw new BuilderError("Compiled ELF exceeds the 128 KB project limit.", 400);
    const metadata = inspectProject(bytes);
    return { bytes, metadata };
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
}

export async function builderHeader() {
  return readFile(join(process.cwd(), "firmware", BOARD, "src", "runtime", "project_builder.h"), "utf8");
}

async function findCompiler() {
  const configured = process.env.PROJECT_CC;
  if (configured && existsSync(configured)) return configured;
  const directory = join(homedir(), "Library", "Arduino15", "packages", "esp32", "tools", "esp-x32");
  if (!existsSync(directory)) return null;
  const versions = (await readdir(directory)).sort().reverse();
  return versions.map((version) => join(directory, version, "bin", "xtensa-esp32s3-elf-gcc")).find(existsSync) ?? null;
}

async function command(file: string, args: string[]) {
  try {
    await runFile(file, args, { timeout: 15_000, maxBuffer: 256 * 1024 });
  } catch (error) {
    const detail = error && typeof error === "object" && "stderr" in error ? String(error.stderr) : "";
    const clean = detail.replaceAll(/\/[^\s:]+\/sketch\.ino/g, "sketch.ino").trim().slice(0, 12_000);
    throw new BuilderError(clean || "The compiler could not build this sketch.", 400);
  }
}

function validate(input: BuilderInput) {
  if (!ID.test(input.id) || input.id === "none") throw new BuilderError("Project ID must use lowercase letters, numbers and hyphens.", 400);
  if (!input.name.trim() || input.name.trim().length > 80) throw new BuilderError("Project name must be 1–80 characters.", 400);
  if (!VERSION.test(input.version)) throw new BuilderError("Version must look like 1.0.0.", 400);
  if (typeof input.description !== "string" || input.description.length > 300) throw new BuilderError("Description must be at most 300 characters.", 400);
  if (typeof input.source !== "string" || !input.source.trim() || Buffer.byteLength(input.source) > MAX_SOURCE_SIZE) throw new BuilderError("Sketch must be between 1 byte and 48 KB.", 400);
  const directives = [...input.source.matchAll(/^\s*#([^\r\n]*)/gm)];
  if (directives.length !== 1 || directives[0][1].trim() !== 'include "project_builder.h"') {
    throw new BuilderError('The only allowed file include is #include "project_builder.h".', 400);
  }
  if (!/\bPROJECT_SKETCH\s*\(\s*\)/.test(input.source)) throw new BuilderError("Add PROJECT_SKETCH() after setup() and loop().", 400);
  if (/\b(?:__asm__|asm)\b|\.incbin\b/.test(input.source)) throw new BuilderError("Inline assembly is not available in Project Builder.", 400);
}

export class BuilderError extends Error {
  constructor(message: string, public readonly status: number) { super(message); }
}
