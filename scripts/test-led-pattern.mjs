// Compiles the firmware LED pattern on the host and checks the web simulator renders the same frames.
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import ts from "typescript";

const root = new URL("..", import.meta.url).pathname;
const led = join(root, "firmware/esp32-s3-lcd-0.85/src/led");
const settingsSource = ts.transpileModule(readFileSync(join(root, "src/lib/device-settings.ts"), "utf8"), { compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2022 } }).outputText;
const settings = await import(`data:text/javascript;base64,${Buffer.from(settingsSource).toString("base64")}`);
globalThis.__ledTest = settings;
const patternSource = ts.transpileModule(readFileSync(join(root, "src/lib/led-pattern.ts"), "utf8"), { compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2022 } }).outputText
  .replace(/import .* from "@\/lib\/device-settings";/, "const { LED_COUNT } = globalThis.__ledTest;");
const pattern = await import(`data:text/javascript;base64,${Buffer.from(patternSource).toString("base64")}`);

const pixels = "ff0000:100:1,00ff00:50:0,0000ff:0:1,ffffff:100:0,a0ff10:75:1,123456:20:0,ff5a00:100:0,00ffff:5:1";
const cases = [];
for (const mode of settings.LED_MODES) for (const speed of [1, 5, 10]) for (const brightness of [1, 4, 8]) for (const now of [0, 137, 999, 1500, 12345, 987654]) cases.push({ mode, speed, brightness, now });

const directory = mkdtempSync(join(tmpdir(), "esp32-led-test-"));
try {
  mkdirSync(join(directory, "driver"));
  writeFileSync(join(directory, "Arduino.h"), "#pragma once\n#include <cstdint>\nstruct rmt_data_t { int level0, duration0, level1, duration1; };\n");
  writeFileSync(join(directory, "driver/gpio.h"), "#pragma once\ntypedef int gpio_num_t;\n");
  const main = join(directory, "main.cpp");
  writeFileSync(main, `#include "led_pattern.h"
#include <cstdio>
#include <string>
int main(int argc, char** argv) {
  const auto pixels = LedPattern::Parse("${pixels}", 8, {1, 2, 3});
  char mode[16]; int speed, brightness; unsigned long now;
  while (scanf("%15s %d %d %lu", mode, &speed, &brightness, &now) == 4) {
    for (const auto& c : LedPattern::Render(mode, pixels, speed, brightness, now)) printf("%d,%d,%d ", c.red, c.green, c.blue);
    printf("%d\\n", LedPattern::Animated(mode, pixels) ? 1 : 0);
  }
}
`);
  const binary = join(directory, "led");
  execFileSync(process.env.CXX || "c++", ["-std=c++17", "-I", directory, "-I", led, main, join(led, "led_pattern.cpp"), "-o", binary], { stdio: "inherit" });
  const output = execFileSync(binary, { input: cases.map((c) => `${c.mode} ${c.speed} ${c.brightness} ${c.now}`).join("\n") + "\n", encoding: "utf8" }).trim().split("\n");
  const parsed = pattern.parsePixels(pixels, 0);
  assert.equal(pattern.formatPixels(parsed), pixels, "Pixel encoding round-trips");
  cases.forEach((c, index) => {
    const firmware = output[index].split(" ");
    const animated = firmware.pop() === "1";
    assert.equal(pattern.isAnimated(c.mode, parsed), animated, `${c.mode}: animated flag`);
    pattern.renderFrame(c.mode, parsed, c.speed, c.brightness, c.now).forEach((pixel, i) => {
      const [r, g, b] = firmware[i].split(",").map(Number);
      // The browser keeps color and intensity apart; the firmware truncates their product to 8 bits.
      const expected = [0, 2, 4].map((offset) => Math.trunc(parseInt(pixel.color.slice(offset, offset + 2), 16) * pixel.intensity));
      for (let k = 0; k < 3; k++) assert.ok(Math.abs(expected[k] - [r, g, b][k]) <= 1, `${JSON.stringify(c)} LED ${i + 1}: firmware ${[r, g, b]} vs simulator ${expected}`);
    });
  });
  assert.deepEqual(pattern.parsePixels("", 1).map((p) => p.color), Array(8).fill("ff0000"), "Empty design uses the LED color for every LED");
  const sanitized = settings.sanitizeSettings({ led_mode: "disco", led_speed: 40, led_pixels: pixels.replace("ff0000:100", "FF0000:250") }, settings.DEFAULT_SETTINGS);
  assert.equal(sanitized.led_mode, "static");
  assert.equal(sanitized.led_speed, 10);
  assert.ok(sanitized.led_pixels.startsWith("ff0000:100:1,"), "Levels are clamped and colors lowercased");
  assert.equal(settings.sanitizeSettings({ led_pixels: "ff0000:1:1" }, settings.DEFAULT_SETTINGS).led_pixels, "", "Designs must describe all 8 LEDs");
  console.log(`✓ LED simulator matches firmware frames in ${cases.length} cases; settings validation passed`);
} finally {
  rmSync(directory, { recursive: true, force: true });
}
