import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFileSync, readdirSync } from "node:fs";
import { join } from "node:path";
const root = new URL("..", import.meta.url).pathname;
const manifest = JSON.parse(readFileSync(join(root, "projects/manifest.json"), "utf8"));
for (const project of manifest.projects) {
  const bytes = readFileSync(join(root, "public", project.path));
  assert.equal(bytes.length, project.size);
  for (const hash of ["md5", "sha256"]) assert.equal(createHash(hash).update(bytes).digest("hex"), project[hash]);
  assert.equal(bytes.subarray(0, 6).toString("hex"), "7f454c460101");
  assert.equal(bytes.readUInt16LE(16), 3, "Module must be ELF ET_DYN");
  assert.equal(bytes.readUInt16LE(18), 94, "Module must target Xtensa");
  const offset = bytes.readUInt32LE(32), count = bytes.readUInt16LE(48), stride = bytes.readUInt16LE(46);
  assert.equal(stride, 40);
  assert.ok(offset + count * stride <= bytes.length);
  for (let i = 0; i < count; ++i) {
    const section = offset + i * stride;
    const type = bytes.readUInt32LE(section + 4), start = bytes.readUInt32LE(section + 16), size = bytes.readUInt32LE(section + 20);
    if (type === 11) { // Dynamic symbol table: no firmware or libc imports allowed.
      for (let symbol = start + 16; symbol < start + size; symbol += 16) {
        assert.ok(bytes.readUInt16LE(symbol + 14) !== 0, `${project.id}: unresolved native import`);
      }
    }
    if (type === 4) { // These modules require only supported relative relocations.
      for (let relocation = start; relocation < start + size; relocation += 12) {
        assert.equal(bytes.readUInt32LE(relocation + 4) & 255, 5, `${project.id}: unsupported relocation`);
      }
    }
  }
  console.log(`✓ ${project.id}: independent Xtensa ELF, no unresolved imports, valid hashes`);
}
function verifyBase(dir) {
  for (const entry of readdirSync(dir, {withFileTypes: true})) {
    const path = join(dir, entry.name);
    if (entry.isDirectory()) verifyBase(path);
    else if (/\.(cpp|c|h)$/.test(entry.name)) {
      assert.ok(!/weather|analog.clock/i.test(readFileSync(path, "utf8")), `Project-specific implementation in base firmware: ${path}`);
    }
  }
}
verifyBase(join(root, "firmware/esp32-s3-lcd-0.85/src"));
console.log("✓ Base firmware contains no Weather or Analog clock implementation");
