import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import ts from "typescript";

const source = ts
  .transpileModule(readFileSync("src/lib/device-client.ts", "utf8"), {
    compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2022 },
  })
  .outputText.replace(/import .* from "@\/lib\/[a-z-]+";/g, "");
const { batteryTrend } = await import(`data:text/javascript;base64,${Buffer.from(source).toString("base64")}`);

const now = Date.now();
const minute = 60_000;
const at = (minutesAgo, battery, batteryMv, charging) => ({
  t: now - minutesAgo * minute,
  battery,
  batteryMv,
  charging,
  rssi: -60,
  heap: 0,
});
const device = (samples, overrides = {}) => ({
  online: true,
  lastSeen: now,
  battery: samples.at(-1).battery,
  batteryMv: samples.at(-1).batteryMv,
  charging: samples.at(-1).charging,
  samples,
  ...overrides,
});

// The screenshot's shape: flat on battery, then a charger is plugged in 28 minutes ago.
const charging = [
  at(180, 20, 3680, false),
  at(60, 19, 3674, false),
  at(28, 45, 3782, true),
  at(10, 80, 3960, true),
  at(0, 95, 4064, true),
];
const trend = batteryTrend(device(charging));
assert.equal(trend.anchor, "charge-start");
assert.equal(trend.minutes, 28, "The span runs from the first charging sample, not from the window");
assert.equal(trend.percent, 50);
assert.equal(trend.millivolts, 282);
assert.equal(trend.direction, 1);
// The jump at the transition itself is an artifact of charge current, so it stays out of the span.
assert.notEqual(trend.percent, 95 - 19);

// Discharging after the charger was pulled: the anchor names that transition instead.
const stopped = [at(90, 90, 4040, true), at(45, 88, 4010, false), at(0, 74, 3890, false)];
const after = batteryTrend(device(stopped));
assert.equal(after.anchor, "charge-stop");
assert.equal(after.minutes, 45);
assert.equal(after.percent, -14);
assert.equal(after.direction, -1);

// A transition older than the six-hour window cannot be claimed as the baseline.
const old = [at(500, 60, 3850, false), at(400, 55, 3820, false), at(30, 50, 3800, false)];
assert.equal(batteryTrend(device(old)).anchor, "window", "An out-of-window transition is not a baseline");

// A board that never reported a charge state still gets a plain window reading.
const flat = [at(200, 70, 3900, undefined), at(120, 66, 3870, undefined), at(0, 60, 3840, undefined)];
assert.equal(batteryTrend(device(flat)).anchor, "window");

// Too short a span, an offline board and a board without a battery all report nothing.
assert.equal(batteryTrend(device([at(4, 50, 3800, true), at(0, 52, 3820, true)])), null);
assert.equal(batteryTrend(device(charging, { online: false })), null);
assert.equal(batteryTrend(device(charging, { battery: -1 })), null);

console.log("✓ Battery trend names its baseline, excludes the charge-transition jump and stays silent when unknown");
