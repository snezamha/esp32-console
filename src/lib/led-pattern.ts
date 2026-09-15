import { LED_COUNT, type LedMode } from "@/lib/device-settings";

/**
 * Browser twin of firmware/<board>/src/led/led_pattern.cpp, so the simulator shows what the ring
 * does. Keep the two in sync.
 */
export type LedPixel = { color: string; level: number; blink: boolean };
export type LedFrame = { color: string; intensity: number }[];

/** Same order as LED_COLORS and the firmware palette. */
export const LED_PALETTE = ["ffffff", "ff0000", "00ff00", "0000ff", "00ffff", "a000ff", "ff5a00"];

export function parsePixels(value: string, fallbackColor: number): LedPixel[] {
  const fallback = LED_PALETTE[fallbackColor] ?? LED_PALETTE[0];
  const items = value ? value.split(",") : [];
  return Array.from({ length: LED_COUNT }, (_, i) => {
    const [color, level, blink] = (items[i] ?? "").split(":");
    return /^[0-9a-f]{6}$/.test(color ?? "")
      ? { color, level: Math.min(100, Math.max(0, Number(level) || 0)), blink: blink === "1" }
      : { color: fallback, level: 100, blink: false };
  });
}

export function formatPixels(pixels: LedPixel[]) {
  return pixels.map((p) => `${p.color}:${Math.round(p.level)}:${p.blink ? 1 : 0}`).join(",");
}

export function isAnimated(mode: LedMode, pixels: LedPixel[]) {
  return mode !== "static" || pixels.some((p) => p.blink && p.level > 0);
}

function hue(value: number) {
  const h = (value - Math.floor(value)) * 6;
  const x = 1 - Math.abs((h % 2) - 1);
  const [r, g, b] = [[1, x, 0], [x, 1, 0], [0, 1, x], [0, x, 1], [x, 0, 1], [1, 0, x]][Math.floor(h) % 6];
  return [r, g, b].map((c) => Math.floor(c * 255).toString(16).padStart(2, "0")).join("");
}

/** `intensity` is the share of full output (0–1) after level, effect and global brightness 1–8. */
export function renderFrame(mode: LedMode, pixels: LedPixel[], speed: number, brightness: number, now: number): LedFrame {
  const count = pixels.length;
  const period = 2200 - Math.min(10, Math.max(1, speed)) * 200;
  const phase = (now % period) / period;
  const blinkOff = phase >= 0.5;
  const global = (2 ** Math.min(8, Math.max(1, brightness)) - 1) / 255;
  const chase = (now % (period * count)) / period;
  const spin = Math.floor(now / Math.max(1, Math.floor(period / count))) % count;
  return pixels.map((_, i) => {
    const source = mode === "spin" ? pixels[(i + count - spin) % count] : pixels[i];
    let level = source.level / 100;
    if (source.blink && blinkOff && mode !== "blink") level = 0;
    if (mode === "blink" && blinkOff) level = 0;
    if (mode === "breathe") level *= 0.5 - 0.5 * Math.cos(phase * 2 * Math.PI);
    if (mode === "chase") {
      let distance = Math.abs(i - chase);
      distance = Math.min(distance, count - distance);
      level *= Math.max(0, 1 - distance / 1.5);
    }
    return { color: mode === "rainbow" ? hue(i / count + phase) : source.color, intensity: level * global };
  });
}
