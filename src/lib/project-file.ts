export type ProjectMetadata = { id: string; name: string; version: string; description: string; board: string; abi: number };
export const MAX_PROJECT_FILE_SIZE = 128 * 1024;

/** Reject oversized browser files before reading them into memory. */
export async function inspectProjectUpload(file: Pick<File, "size" | "arrayBuffer">): Promise<ProjectMetadata> {
  if (!file.size || file.size > MAX_PROJECT_FILE_SIZE) {
    throw new Error("Project file must be between 1 byte and 128 KB.");
  }
  return inspectProject(new Uint8Array(await file.arrayBuffer()));
}

/** A project is one ELF file, with its identity embedded in the .project section. */
export function inspectProject(bytes: Uint8Array): ProjectMetadata {
  const fail = (): never => { throw new Error("Invalid project file. Choose a standalone ESP32 project .elf built for ABI 2 through 8."); };
  if (bytes.length < 52 || bytes.length > MAX_PROJECT_FILE_SIZE) return fail();
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (v.getUint32(0, false) !== 0x7f454c46 || bytes[4] !== 1 || bytes[5] !== 1 || v.getUint16(16, true) !== 3 || v.getUint16(18, true) !== 94) return fail();
  const offset = v.getUint32(32, true), count = v.getUint16(48, true), names = v.getUint16(50, true);
  if (!count || count > 128 || names >= count || v.getUint16(46, true) !== 40 || offset + count * 40 > bytes.length) return fail();
  const section = (n: number, field: number) => v.getUint32(offset + n * 40 + field, true);
  const nameStart = section(names, 16), nameSize = section(names, 20);
  if (nameStart + nameSize > bytes.length) return fail();
  let metadata: ProjectMetadata | undefined;
  for (let i = 0; i < count; i++) {
    const start = section(i, 16), size = section(i, 20), type = section(i, 4), nameOffset = section(i, 0);
    if (size > 128 * 1024 || section(i, 24) >= count || (type !== 8 && start + size > bytes.length) || nameOffset >= nameSize) return fail();
    const end = bytes.indexOf(0, nameStart + nameOffset);
    if (end < 0 || end >= nameStart + nameSize) return fail();
    const name = new TextDecoder().decode(bytes.subarray(nameStart + nameOffset, end));
    if (type === 11) {
      if (size % 16) return fail();
      for (let j = start + 16; j < start + size; j += 16) if (v.getUint16(j + 14, true) === 0) return fail();
    }
    if (type === 4) {
      if (size % 12) return fail();
      for (let j = start; j < start + size; j += 12) if ((v.getUint32(j + 4, true) & 255) !== 5) return fail();
    }
    if (name === ".project") {
      if (size > 4096 || metadata) return fail();
      try { metadata = JSON.parse(new TextDecoder().decode(bytes.subarray(start, start + size))); } catch { return fail(); }
    }
  }
  if (!metadata || !/^[a-z0-9][a-z0-9-]{0,47}$/.test(metadata.id) || metadata.id === "none" || typeof metadata.name !== "string" || !metadata.name.length || metadata.name.length > 80 || !/^\d+\.\d+\.\d+$/.test(metadata.version) || metadata.board !== "esp32-s3-lcd-0.85" || ![2, 3, 4, 5, 6, 7, 8].includes(metadata.abi)) return fail();
  return { ...metadata, description: typeof metadata.description === "string" ? metadata.description.slice(0, 300) : "" };
}
