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
  if (project.assets) {
    assert.equal(project.abi, 3, `${project.id}: SD card assets require ABI 3`);
    const index = readFileSync(join(root, "public", project.assets.path));
    assert.equal(index.length, project.assets.size);
    assert.equal(createHash("sha256").update(index).digest("hex"), project.assets.sha256);
    const base = join(root, "public", project.assets.path.replace(/assets\.txt$/, "assets"));
    let total = 0, files = 0;
    for (const line of index.toString("utf8").split("\n").filter(Boolean)) {
      const [name, size, sha256] = line.split("|");
      assert.ok(!name.includes("..") && !name.startsWith("/"), `${project.id}: unsafe asset name ${name}`);
      const bytes = readFileSync(join(base, name));
      assert.equal(bytes.length, Number(size), `${project.id}: ${name} size`);
      assert.equal(createHash("sha256").update(bytes).digest("hex"), sha256, `${project.id}: ${name} hash`);
      if (/\.(img|vid)$/.test(name)) {
        assert.equal(bytes.subarray(0, 4).toString("ascii"), "EVM1", `${project.id}: ${name} media header`);
        assert.equal(bytes.length, 16 + bytes.readUInt16LE(4) * bytes.readUInt16LE(6) * bytes.readUInt16LE(8) * 2, `${project.id}: ${name} media length`);
      }
      total += bytes.length; files++;
    }
    assert.deepEqual(project.storage, { sd: true, bytes: total, files }, `${project.id}: storage summary`);
  } else assert.equal(project.abi, 2, `${project.id}: projects without SD card assets stay on ABI 2`);
  console.log(`✓ ${project.id}: independent Xtensa ELF, no unresolved imports, valid hashes${project.assets ? `, ${project.storage.files} verified SD card files` : ""}`);
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
