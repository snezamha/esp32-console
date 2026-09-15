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

const SIZE = 240;
const CENTER = SIZE / 2;
const RADIUS = 92;

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
        <svg viewBox={`0 0 ${SIZE} ${SIZE}`} className="w-56 max-w-full shrink-0" role="group" aria-label="LED ring simulator">
          <circle cx={CENTER} cy={CENTER} r={112} className="fill-zinc-800 dark:fill-zinc-950" />
          <circle cx={CENTER} cy={CENTER} r={104} className="fill-zinc-900 stroke-zinc-700" strokeWidth={1} />
          <rect x={CENTER - 42} y={CENTER - 42} width={84} height={84} rx={8} className="fill-black stroke-zinc-700" />
          <text x={CENTER} y={CENTER + 4} textAnchor="middle" className="fill-zinc-500 text-[10px]">Screen</text>
          {pixels.map((_, i) => {
            const angle = (i / LED_COUNT) * 2 * Math.PI - Math.PI / 2;
            const x = CENTER + RADIUS * Math.cos(angle);
            const y = CENTER + RADIUS * Math.sin(angle);
            const lit = value.led_on ? frame[i] : { color: "000000", intensity: 0 };
            // Scaled for the screen: the real LEDs are far brighter than a monitor at low levels.
            const shown = lit.intensity > 0 ? 0.25 + 0.75 * Math.sqrt(lit.intensity) : 0;
            const isSelected = selected.includes(i);
            return (
              <g key={i} onClick={() => toggle(i)} className="cursor-pointer" role="checkbox" aria-checked={isSelected} aria-label={`LED ${i + 1}`} tabIndex={0} onKeyDown={(e) => { if (e.key === " " || e.key === "Enter") { e.preventDefault(); toggle(i); } }}>
                {shown > 0 && <circle cx={x} cy={y} r={20} fill={`#${lit.color}`} opacity={shown * 0.35} />}
                <circle cx={x} cy={y} r={11} fill={`#${lit.color}`} fillOpacity={shown} className="stroke-zinc-600" strokeWidth={1} />
                {isSelected && <circle cx={x} cy={y} r={15} fill="none" className="stroke-blue-400" strokeWidth={2.5} />}
                <text x={CENTER + (RADIUS - 26) * Math.cos(angle)} y={CENTER + (RADIUS - 26) * Math.sin(angle) + 3} textAnchor="middle" className="fill-zinc-500 text-[9px]">{i + 1}</text>
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
