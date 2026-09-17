"use client";

import { Button } from "@headlessui/react";
import { useEffect, useState } from "react";
import { Select } from "@/components/Select";
import { Sheet, Slider, Toggle, accentButton, inputClass, secondaryButton } from "@/components/ui";
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

// Eight LEDs around the display: three on each side, with each corner shared by two sides.
// LED 1 is upper left and numbering follows the perimeter clockwise (USB at the bottom).
const LED_POSITIONS = [
  { x: 55, y: 42, labelX: 55, labelY: 24 },
  { x: 120, y: 42, labelX: 120, labelY: 24 },
  { x: 185, y: 42, labelX: 185, labelY: 24 },
  { x: 185, y: 105, labelX: 208, labelY: 108 },
  { x: 185, y: 168, labelX: 185, labelY: 193 },
  { x: 120, y: 168, labelX: 120, labelY: 193 },
  { x: 55, y: 168, labelX: 55, labelY: 193 },
  { x: 55, y: 105, labelX: 32, labelY: 108 },
] as const;

/** Shared ring simulator for Configure and the Board API workbench. */
export function LedRingDesigner({ value, onChange }: { value: LedSettings; onChange: (next: Partial<LedSettings>) => void }) {
  const pixels = parsePixels(value.led_pixels, value.led_color);
  const [editing, setEditing] = useState<number | null>(null);
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
  const editPixel = (index: number, patch: Partial<LedPixel>) =>
    onChange({ led_on: true, led_pixels: formatPixels(pixels.map((pixel, i) => i === index ? { ...pixel, ...patch } : pixel)) });
  const selectedPixel = editing === null ? null : pixels[editing];
  const rgb = selectedPixel ? [0, 2, 4].map((start) => parseInt(selectedPixel.color.slice(start, start + 2), 16)) : [];
  const editChannel = (channel: number, input: number) => {
    if (editing === null || !Number.isFinite(input)) return;
    const next = [...rgb];
    next[channel] = Math.max(0, Math.min(255, Math.round(input)));
    editPixel(editing, { color: next.map((part) => part.toString(16).padStart(2, "0")).join("") });
  };

  return (
    <div className="space-y-4">
      <div className="flex flex-col items-center gap-4 sm:flex-row sm:items-start">
        <svg viewBox={`0 0 ${BOARD_WIDTH} ${BOARD_HEIGHT}`} className="w-60 max-w-full shrink-0" role="group" aria-label="LED layout on the ESP32-S3-LCD-0.85 board; select an LED to edit it">
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
            const { x, y, labelX, labelY } = LED_POSITIONS[i];
            const lit = value.led_on ? frame[i] : { color: "000000", intensity: 0 };
            // Scaled for the screen: the real LEDs are far brighter than a monitor at low levels.
            const shown = lit.intensity > 0 ? 0.25 + 0.75 * Math.sqrt(lit.intensity) : 0;
            return (
              <g key={i} onClick={() => setEditing(i)} className="cursor-pointer outline-none" role="button" aria-haspopup="dialog" aria-label={`Edit LED ${i + 1}, color #${pixels[i].color}`} tabIndex={0} onKeyDown={(e) => { if (e.key === " " || e.key === "Enter") { e.preventDefault(); setEditing(i); } }}>
                <circle cx={x} cy={y} r={20} fill="transparent" />
                {shown > 0 && <circle cx={x} cy={y} r={20} fill={`#${lit.color}`} opacity={shown * 0.35} />}
                <rect x={x - 8} y={y - 8} width={16} height={16} rx={2} className="fill-zinc-200 stroke-zinc-500" strokeWidth={1} />
                <rect x={x - 5} y={y - 5} width={10} height={10} rx={1.5} fill={`#${lit.color}`} fillOpacity={shown} className="stroke-zinc-600" strokeWidth={0.75} />
                {editing === i && <rect x={x - 12} y={y - 12} width={24} height={24} rx={6} fill="none" className="stroke-blue-400" strokeWidth={2.5} />}
                <text x={labelX} y={labelY} textAnchor="middle" className="fill-slate-400 text-[9px]">{i + 1}</text>
              </g>
            );
          })}
        </svg>
        <div className="w-full min-w-0 space-y-3">
          <p className="text-xs text-zinc-500">Eight LEDs surround the display. Each side has three LEDs, counting its corners. Tap a light or its numbered button to edit it.</p>
          <div className="grid grid-cols-4 gap-2">
            {pixels.map((pixel, index) => <Button key={index} onClick={() => setEditing(index)} className={secondaryButton + " flex min-w-0 flex-col items-center gap-1 px-1 py-2 text-xs"} aria-label={`Edit LED ${index + 1}`}>
              <span className="size-5 rounded-full border border-zinc-300 dark:border-zinc-600" style={{ backgroundColor: `#${pixel.color}` }} />
              <span>LED {index + 1}</span>
              <span className="text-[10px] text-zinc-500">{pixel.level}%</span>
            </Button>)}
          </div>
          <p className="text-xs text-zinc-500">LED 1 is at the upper left. Numbers run clockwise.</p>
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
      <Sheet
        open={editing !== null}
        onClose={() => setEditing(null)}
        title={`LED ${editing === null ? "" : editing + 1}`}
        subtitle="Choose this LED's RGB color, intensity and blink. Changes appear in the preview immediately."
        footer={<>
          <Button onClick={() => setEditing((current) => current === null ? null : (current + LED_COUNT - 1) % LED_COUNT)} className={secondaryButton + " h-10 px-3"}>Previous</Button>
          <Button onClick={() => setEditing((current) => current === null ? null : (current + 1) % LED_COUNT)} className={secondaryButton + " h-10 px-3"}>Next</Button>
          <Button onClick={() => setEditing(null)} className={accentButton + " h-10 px-4"}>Done</Button>
        </>}
      >
        {selectedPixel && editing !== null && <div className="space-y-5 pb-1">
          <div className="flex items-center gap-3 rounded-xl border border-zinc-200 p-3 dark:border-zinc-800">
            <span className="size-11 shrink-0 rounded-full border border-zinc-300 dark:border-zinc-600" style={{ backgroundColor: `#${selectedPixel.color}` }} />
            <div><p className="text-sm font-medium">LED {editing + 1}</p><p className="font-mono text-xs text-zinc-500">#{selectedPixel.color.toUpperCase()} · {selectedPixel.level}%{selectedPixel.blink ? " · blinking" : ""}</p></div>
          </div>
          <div className="space-y-2">
            <p className="text-xs font-medium text-zinc-500">Quick colors</p>
            <div className="flex flex-wrap gap-2">{LED_PALETTE.map((color) => <Button key={color} onClick={() => editPixel(editing, { color })} aria-label={`Set LED ${editing + 1} to #${color}`} aria-pressed={selectedPixel.color === color} className={`size-10 rounded-full border-2 ${selectedPixel.color === color ? "border-blue-500 ring-2 ring-blue-500/30" : "border-zinc-300 dark:border-zinc-600"}`} style={{ backgroundColor: `#${color}` }} />)}</div>
          </div>
          <label className="flex items-center gap-3 text-sm"><span className="min-w-20">Custom RGB</span><input type="color" value={`#${selectedPixel.color}`} onChange={(event) => editPixel(editing, { color: event.target.value.slice(1).toLowerCase() })} className="size-11 cursor-pointer" /><span className="font-mono text-xs text-zinc-500">#{selectedPixel.color.toUpperCase()}</span></label>
          <div className="grid grid-cols-3 gap-3">{(["Red", "Green", "Blue"] as const).map((name, channel) => <label key={name} className="space-y-1 text-xs font-medium text-zinc-500"><span>{name}</span><input type="number" min={0} max={255} value={rgb[channel]} onChange={(event) => editChannel(channel, Number(event.target.value))} className={inputClass + " tabular-nums"} /></label>)}</div>
          <Slider label="LED intensity" value={selectedPixel.level} min={0} max={100} step={1} unit="%" onChange={(level) => editPixel(editing, { level })} />
          <Toggle label="Blink this LED" checked={selectedPixel.blink} onChange={(blink) => editPixel(editing, { blink })} />
        </div>}
      </Sheet>
    </div>
  );
}
