import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import ts from "typescript";
let user = null, calls = 0, query;
const originalFetch = globalThis.fetch;
globalThis.__locationTestAuth = { getUser: async () => user, unauthorized: () => new Response(null, { status: 401 }) };
const source = ts.transpileModule(readFileSync("src/app/api/weather/locations/route.ts", "utf8"), { compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.ESNext } }).outputText.replace(/import .* from "@\/lib\/auth";/, "const { getUser, unauthorized } = globalThis.__locationTestAuth;");
try {
  const { GET } = await import(`data:text/javascript;base64,${Buffer.from(source).toString("base64")}`);
  globalThis.fetch = async (url) => {
    calls++; query = new URL(url);
    return Response.json({ results: [{ id: 1, name: "Tehran", latitude: 35.6892, longitude: 51.389, country: "Iran", timezone: "Asia/Tehran" }, { id: 2, name: "Invalid", latitude: 95, longitude: 10 }] });
  };
  const request = (q) => new Request(`http://test/api/weather/locations?q=${encodeURIComponent(q)}`);
  assert.equal((await GET(request("Berlin"))).status, 401);
  assert.equal(calls, 0);
  user = { id: "owner" };
  assert.deepEqual(await (await GET(request("a"))).json(), { locations: [] });
  assert.equal(calls, 0);
  const result = await (await GET(request("تهران"))).json();
  assert.equal(result.locations.length, 1);
  assert.equal(result.locations[0].latitude, 35.6892);
  assert.equal(query.searchParams.get("name"), "تهران");
  assert.equal(query.searchParams.get("language"), "fa");
  globalThis.fetch = async () => { throw new Error("Connection unavailable"); };
  assert.equal((await GET(request("Berlin"))).status, 502);
  console.log("✓ City search authentication, Unicode queries, coordinate validation and outage responses passed");
} finally { globalThis.fetch = originalFetch; delete globalThis.__locationTestAuth; }
