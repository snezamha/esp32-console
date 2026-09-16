import manifest from "../../projects/manifest.json";
import { TIME_ZONES, type DeviceSettings } from "@/lib/device-settings";

export type ProjectLocation = { latitude: number; longitude: number; label?: string };
export type ProjectSettingValue = string | number | boolean | ProjectLocation;
export type ProjectConfig = Record<string, ProjectSettingValue>;
export type ProjectConfigs = Record<string, ProjectConfig>;
export type ProjectSettingDefinition = {
  key: string;
  type: "boolean" | "select" | "timezone" | "number" | "location" | "text";
  label: string;
  default: ProjectSettingValue;
  options?: { value: string; label: string }[];
  min?: number;
  max?: number;
  step?: number;
  /** "text" only: trimmed and truncated to this length; also keeps the on-device data payload bounded. */
  maxLength?: number;
  placeholder?: string;
};
// Declared rather than inferred from the manifest JSON: inference breaks as soon as catalog entries
// differ in shape (for example one project with SD card assets and one without).
export type ProjectDefinition = {
  id: string;
  name: string;
  version: string;
  description: string;
  board: string;
  abi: number;
  path: string;
  size: number;
  md5: string;
  sha256: string;
  category: string;
  icon: string;
  accent: string;
  minimumFirmware: string;
  capabilities: string[];
  settings: ProjectSettingDefinition[];
  /** Present when the project installs files onto the board's SD card (ABI 3). */
  storage?: ProjectStorage;
  assets?: { path: string; size: number; sha256: string };
};
export type ProjectStorage = { sd: true; bytes: number; files: number };

export const PROJECT_DEFINITIONS = manifest.projects as unknown as ProjectDefinition[];

export function projectDefinition(id: string) {
  return PROJECT_DEFINITIONS.find((project) => project.id === id);
}

export function defaultProjectConfig(project: ProjectDefinition): ProjectConfig {
  return Object.fromEntries(project.settings.map((setting) => [setting.key, structuredClone(setting.default)]));
}

export function projectConfigsWithDefaults(raw: unknown, legacy?: DeviceSettings): ProjectConfigs {
  const source = raw && typeof raw === "object" && !Array.isArray(raw) ? raw as Record<string, unknown> : {};
  return Object.fromEntries(PROJECT_DEFINITIONS.map((project) => {
    const fallback = defaultProjectConfig(project);
    if (project.id === "weather" && legacy) {
      fallback.location = { latitude: legacy.weather_lat / 10000, longitude: legacy.weather_lon / 10000 };
      fallback.unit = legacy.weather_unit;
    }
    if (project.id === "analog-clock" && legacy) {
      fallback.timezone = legacy.tz;
      fallback.seconds = legacy.project_seconds;
    }
    return [project.id, sanitizeProjectConfig(project.id, source[project.id], fallback)];
  }));
}

export function sanitizeProjectConfig(projectId: string, input: unknown, base?: ProjectConfig): ProjectConfig {
  const project = projectDefinition(projectId);
  if (!project) throw new Error("Unknown project.");
  const source = input && typeof input === "object" && !Array.isArray(input) ? input as Record<string, unknown> : {};
  const output = { ...(base ?? defaultProjectConfig(project)) };
  for (const setting of project.settings) {
    const value = source[setting.key];
    if (value === undefined) continue;
    if (setting.type === "boolean") {
      if (typeof value === "boolean") output[setting.key] = value;
    } else if (setting.type === "select") {
      if (typeof value === "string" && setting.options?.some((option) => option.value === value)) output[setting.key] = value;
    } else if (setting.type === "timezone") {
      if (typeof value === "string" && TIME_ZONES.some((zone) => zone.id === value)) output[setting.key] = value;
    } else if (setting.type === "number") {
      const number = Number(value);
      if (Number.isFinite(number)) output[setting.key] = Math.min(setting.max ?? number, Math.max(setting.min ?? number, number));
    } else if (setting.type === "text") {
      if (typeof value === "string") output[setting.key] = value.trim().slice(0, setting.maxLength ?? 256);
    } else if (setting.type === "location" && value && typeof value === "object" && !Array.isArray(value)) {
      const location = value as Record<string, unknown>;
      const latitude = Number(location.latitude), longitude = Number(location.longitude);
      if (Number.isFinite(latitude) && Number.isFinite(longitude) && Math.abs(latitude) <= 90 && Math.abs(longitude) <= 180) {
        output[setting.key] = { latitude, longitude, ...(typeof location.label === "string" ? { label: location.label.trim().slice(0, 120) } : {}) };
      }
    }
  }
  return output;
}
