import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import ts from "typescript";

const source = ts.transpileModule(readFileSync("src/lib/owner-id.ts", "utf8"), {
  compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.ESNext },
}).outputText;
const { ownerIdForEmail } = await import(`data:text/javascript;base64,${Buffer.from(source).toString("base64")}`);

const owner = ownerIdForEmail(" User@Example.COM ");
assert.equal(owner, ownerIdForEmail("user@example.com"));
assert.match(owner, /^google-email:[a-f0-9]{64}$/);
assert.notEqual(owner, ownerIdForEmail("someone-else@example.com"));
assert.equal(ownerIdForEmail("  "), "");

console.log("✓ Stable owner keys normalize email, hide plaintext and separate accounts");
