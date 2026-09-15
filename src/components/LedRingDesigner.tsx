"use client";

import { Button } from "@headlessui/react";
import { useEffect, useState } from "react";
import { Select } from "@/components/Select";
import { Slider, Toggle, secondaryButton } from "@/components/ui";
import { LED_COUNT, LED_MODES, type DeviceSettings, type LedMode } from "@/lib/device-settings";
import { LED_PALETTE, formatPixels, isAnimated, parsePixels, renderFrame, type LedPixel } from "@/lib/led-pattern";

type LedSettings = Pick<DeviceSettings, "led_on" | "led_mode" | "led_speed" | "led_pixels" | "led_color" | "led_brightness">;

const MODE_LABELS: Record<LedMode, string> = {
  static: "Static · blink only where set",
  blink: "Blink all",
  breathe: "Breathe",
  spin: "Spin colors around the ring",
  chase: "Chasing dot",
  rainbow: "Rainbow",
};

const PRESETS: { label: string; mode: LedMode; pixels: (i: number) => LedPixel }[] = [
  { label: "White", mode: "static", pixels: () => ({ color: "ffffff", level: 100, blink: false }) },
  { label: "Rainbow", mode: "rainbow", pixels: () => ({ color: "ffffff", level: 100, blink: false }) },
  { label: "Police", mode: "static", pixels: (i) => ({ color: i % 2 ? "0000ff" : "ff0000", level: 100, blink: i % 4 < 2 }) },
  { label: "Compass", mode: "static", pixels: (i) => ({ color: i === 0 ? "ff0000" : "00ffff", level: i === 0 ? 100 : 15, blink: false }) },
  { label: "Comet", mode: "chase", pixels: () => ({ color: "ff5a00", level: 100, blink: false }) },
];

const BOARD_WIDTH = 240;
const BOARD_HEIGHT = 210;

// Physical positions on the 46 x 40 mm board, viewed from the display side with USB at the bottom.
// The data chain starts at the upper-left LED and follows the perimeter clockwise.
const LED_POSITIONS = [
  { x: 55, y: 38 },
  { x: 98, y: 38 },
  { x: 142, y: 38 },
  { x: 185, y: 38 },
  { x: 185, y: 172 },
  { x: 142, y: 172 },
  { x: 98, y: 172 },
  { x: 55, y: 172 },
] as const;

/** Ring simulator: pick LEDs, give each a color, level and blink, and choose a ring effect. */
export function LedRingDesigner({ value, onChange }: { value: LedSettings; onChange: (next: Partial<LedSettings>) => void }) {
  const pixels = parsePixels(value.led_pixels, value.led_color);
  const [selected, setSelected] = useState<number[]>([0]);
  const [now, setNow] = useState(0);
  const animated = value.led_on && isAnimated(value.led_mode, pixels);

  useEffect(() => {
    if (!animated) return;
    let frame = requestAnimationFrame(function tick(time) {
      setNow(time);
      frame = requestAnimationFrame(tick);
    });
    return () => cancelAnimationFrame(frame);
  }, [animated]);

  const frame = renderFrame(value.led_mode, pixels, value.led_speed, value.led_brightness, now);
  const first = pixels[selected[0]] ?? pixels[0];
  const update = (patch: Partial<LedPixel>) =>
    onChange({ led_on: true, led_pixels: formatPixels(pixels.map((pixel, i) => (selected.includes(i) ? { ...pixel, ...patch } : pixel))) });
  const toggle = (index: number) =>
    setSelected((current) => (current.includes(index) ? current.filter((i) => i !== index) : [...current, index].sort((a, b) => a - b)));

  return (
    <div className="space-y-4">
      <div className="flex flex-col items-center gap-3 sm:flex-row sm:items-start">
        <svg viewBox={`0 0 ${BOARD_WIDTH} ${BOARD_HEIGHT}`} className="w-60 max-w-full shrink-0" role="group" aria-label="LED layout on the ESP32-S3-LCD-0.85 board">
          <defs>
            <linearGradient id="pcb" x1="0" y1="0" x2="0" y2="1">
              <stop offset="0" stopColor="#0b2940" />
              <stop offset="1" stopColor="#061a2b" />
            </linearGradient>
          </defs>
          <rect x={10} y={8} width={220} height={194} rx={12} fill="url(#pcb)" className="stroke-slate-600" strokeWidth={1.5} />
          {[{ x: 23, y: 21 }, { x: 217, y: 21 }, { x: 23, y: 189 }, { x: 217, y: 189 }].map((hole) => (
            <g key={`${hole.x}-${hole.y}`}>
              <circle cx={hole.x} cy={hole.y} r={8} className="fill-slate-300 stroke-slate-500" strokeWidth={1.5} />
              <circle cx={hole.x} cy={hole.y} r={4} className="fill-slate-700" />
            </g>
          ))}
          <rect x={72} y={54} width={96} height={102} rx={7} className="fill-slate-800 stroke-slate-500" strokeWidth={1} />
          <rect x={81} y={64} width={78} height={82} rx={2} className="fill-black stroke-zinc-600" />
          <text x={120} y={108} textAnchor="middle" className="fill-zinc-500 text-[9px]">128 × 128</text>
          <path d="M108 202h24v5h-24z" className="fill-slate-400 stroke-slate-600" />
          {pixels.map((_, i) => {
            const { x, y } = LED_POSITIONS[i];
            const lit = value.led_on ? frame[i] : { color: "000000", intensity: 0 };
            // Scaled for the screen: the real LEDs are far brighter than a monitor at low levels.
            const shown = lit.intensity > 0 ? 0.25 + 0.75 * Math.sqrt(lit.intensity) : 0;
            const isSelected = selected.includes(i);
            return (
              <g key={i} onClick={() => toggle(i)} className="cursor-pointer" role="checkbox" aria-checked={isSelected} aria-label={`LED ${i + 1}`} tabIndex={0} onKeyDown={(e) => { if (e.key === " " || e.key === "Enter") { e.preventDefault(); toggle(i); } }}>
                {shown > 0 && <circle cx={x} cy={y} r={20} fill={`#${lit.color}`} opacity={shown * 0.35} />}
                <rect x={x - 8} y={y - 8} width={16} height={16} rx={2} className="fill-zinc-200 stroke-zinc-500" strokeWidth={1} />
                <rect x={x - 5} y={y - 5} width={10} height={10} rx={1.5} fill={`#${lit.color}`} fillOpacity={shown} className="stroke-zinc-600" strokeWidth={0.75} />
                {isSelected && <rect x={x - 12} y={y - 12} width={24} height={24} rx={6} fill="none" className="stroke-blue-400" strokeWidth={2.5} />}
                <text x={x} y={i < 4 ? y + 22 : y - 17} textAnchor="middle" className="fill-slate-400 text-[8px]">{i + 1}</text>
              </g>
            );
          })}
        </svg>
        <div className="w-full min-w-0 space-y-3">
          <p className="text-xs text-zinc-500">
            Tap LEDs to select them. {selected.length ? `${selected.length} selected: ${selected.map((i) => i + 1).join(", ")}` : "None selected."} LED 1 is the ring’s first LED; numbering runs clockwise.
          </p>
          <div className="flex flex-wrap gap-2">
            <Button onClick={() => setSelected(pixels.map((_, i) => i))} className={secondaryButton + " h-8 px-3 text-xs"}>Select all</Button>
            <Button onClick={() => setSelected([])} className={secondaryButton + " h-8 px-3 text-xs"}>Clear</Button>
          </div>
          <div className={selected.length ? "space-y-3" : "pointer-events-none space-y-3 opacity-50"}>
            <div className="flex flex-wrap items-center gap-2">
              {LED_PALETTE.map((color) => (
                <button key={color} type="button" aria-label={`Color #${color}`} onClick={() => update({ color })} className="size-7 rounded-full border border-zinc-300 dark:border-zinc-600" style={{ background: `#${color}` }} />
              ))}
              <label className="flex items-center gap-1 text-xs text-zinc-500">
                <input type="color" value={`#${first.color}`} onChange={(e) => update({ color: e.target.value.slice(1).toLowerCase() })} className="size-8 cursor-pointer rounded border-0 bg-transparent" />
                Custom
              </label>
            </div>
            <Slider label="Selected LED level" value={first.level} min={0} max={100} step={5} unit="%" onChange={(level) => update({ level })} />
            <Toggle label="Blink selected LEDs" checked={first.blink} onChange={(blink) => update({ blink })} />
          </div>
        </div>
      </div>
      <Select
        label="Ring effect"
        value={value.led_mode}
        options={[...LED_MODES]}
        onChange={(led_mode) => onChange({ led_on: true, led_mode })}
        getKey={(mode) => mode}
        renderValue={(mode) => MODE_LABELS[mode]}
      />
      <Slider label="Effect and blink speed" value={value.led_speed} min={1} max={10} step={1} unit="/10" onChange={(led_speed) => onChange({ led_speed })} />
      <div className="flex flex-wrap gap-2">
        {PRESETS.map((preset) => (
          <Button
            key={preset.label}
            onClick={() => onChange({ led_on: true, led_mode: preset.mode, led_pixels: formatPixels(Array.from({ length: LED_COUNT }, (_, i) => preset.pixels(i))) })}
            className={secondaryButton + " h-8 px-3 text-xs"}
          >
            {preset.label}
          </Button>
        ))}
      </div>
    </div>
  );
}
