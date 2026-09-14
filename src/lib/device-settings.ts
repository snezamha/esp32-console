/**
 * Board settings that the console can change. Keys match the `set.<key>=<value>` lines
 * the firmware understands (firmware/<board>/src/services/console_client.cpp).
 */
export type DeviceSettings = {
  project: string;
  weather_lat: number;
  weather_lon: number;
  weather_unit: "celsius" | "fahrenheit";
  project_seconds: boolean;
  volume: number;
  brightness: number;
  theme: "dark" | "light";
  button_sound: boolean;
  led_on: boolean;
  led_feedback: boolean;
  led_brightness: number;
  led_color: number;
  sleep_s: number;
  power_off_s: number;
  rotate: boolean;
  ble_on: boolean;
  clock_on: boolean;
  tz: string;
  bat_pct: boolean;
};

export const DEFAULT_SETTINGS: DeviceSettings = {
  project: "none",
  weather_lat: 525200,
  weather_lon: 134050,
  weather_unit: "celsius",
  project_seconds: true,
  volume: 70,
  brightness: 100,
  theme: "dark",
  button_sound: true,
  led_on: false,
  led_feedback: false,
  led_brightness: 4,
  led_color: 0,
  sleep_s: 60,
  power_off_s: 0,
  rotate: false,
  ble_on: true,
  clock_on: true,
  tz: "Europe/Berlin",
  bat_pct: true,
};

export const LED_COLORS = ["White", "Red", "Green", "Blue", "Cyan", "Purple", "Orange"];
export const SLEEP_OPTIONS = [10, 30, 60, 300, 0];
export const POWER_OFF_OPTIONS = [60, 300, 600, 0];

/** Same list as firmware/<board>/src/services/time_zones.h. */
export const TIME_ZONES: { id: string; label: string }[] = [
  { id: "Europe/Berlin", label: "Berlin" },
  { id: "Europe/London", label: "London" },
  { id: "Europe/Paris", label: "Paris" },
  { id: "Europe/Istanbul", label: "Istanbul" },
  { id: "Europe/Moscow", label: "Moscow" },
  { id: "Asia/Tehran", label: "Tehran" },
  { id: "Asia/Dubai", label: "Dubai" },
  { id: "Asia/Kabul", label: "Kabul" },
  { id: "Asia/Karachi", label: "Karachi" },
  { id: "Asia/Kolkata", label: "Delhi" },
  { id: "Asia/Shanghai", label: "Shanghai" },
  { id: "Asia/Tokyo", label: "Tokyo" },
  { id: "Australia/Sydney", label: "Sydney" },
  { id: "UTC", label: "UTC" },
  { id: "America/Sao_Paulo", label: "Sao Paulo" },
  { id: "America/New_York", label: "New York" },
  { id: "America/Chicago", label: "Chicago" },
  { id: "America/Los_Angeles", label: "Los Angeles" },
];

const clamp = (value: number, min: number, max: number) => Math.min(max, Math.max(min, value));

/** Coerces untrusted input (form fields from the board, JSON from the panel) into valid settings. */
export function sanitizeSettings(input: Record<string, unknown>, base: DeviceSettings): DeviceSettings {
  const out = { ...base };
  const num = (key: keyof DeviceSettings, min: number, max: number) => {
    const value = Number(input[key]);
    if (input[key] !== undefined && input[key] !== "" && Number.isFinite(value)) {
      (out[key] as number) = clamp(Math.round(value), min, max);
    }
  };
  const bool = (key: keyof DeviceSettings) => {
    const value = input[key];
    if (typeof value === "boolean") (out[key] as boolean) = value;
    else if (value === "1" || value === "true") (out[key] as boolean) = true;
    else if (value === "0" || value === "false") (out[key] as boolean) = false;
  };
  const oneOf = (key: "sleep_s" | "power_off_s", options: number[]) => {
    const value = Number(input[key]);
    if (input[key] !== undefined && options.includes(value)) out[key] = value;
  };

  if (typeof input.project === "string" && /^[a-z0-9][a-z0-9-]{0,47}$/.test(input.project)) out.project = input.project;
  if (input.weather_unit === "celsius" || input.weather_unit === "fahrenheit") out.weather_unit = input.weather_unit;
  bool("project_seconds");
  num("weather_lat", -900000, 900000);
  num("weather_lon", -1800000, 1800000);
  num("volume", 0, 100);
  num("brightness", 10, 100);
  if (input.theme === "dark" || input.theme === "light") out.theme = input.theme;
  bool("button_sound");
  bool("led_on");
  bool("led_feedback");
  num("led_brightness", 1, 8);
  num("led_color", 0, LED_COLORS.length - 1);
  oneOf("sleep_s", SLEEP_OPTIONS);
  oneOf("power_off_s", POWER_OFF_OPTIONS);
  bool("rotate");
  bool("ble_on");
  bool("clock_on");
  if (typeof input.tz === "string" && TIME_ZONES.some((z) => z.id === input.tz)) out.tz = input.tz;
  bool("bat_pct");
  return out;
}

export function formatSettingValue(value: DeviceSettings[keyof DeviceSettings]) {
  return typeof value === "boolean" ? (value ? "1" : "0") : String(value);
}
