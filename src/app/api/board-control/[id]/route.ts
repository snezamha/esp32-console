import { controlDevice, controlRequestStatus, updateControlDevice, type ControlAudio, type ControlRequest } from "@/lib/device-store";
import { LED_COUNT, sanitizeSettings, type DeviceSettings } from "@/lib/device-settings";
import { parsePixels } from "@/lib/led-pattern";
import { randomBytes } from "node:crypto";
import { db } from "@/lib/db";

const fields = new Set<keyof DeviceSettings>([
  "theme", "brightness", "volume", "led_on", "led_feedback", "led_brightness", "led_color", "led_mode", "led_speed", "led_pixels",
  "button_sound", "sleep_s", "power_off_s", "rotate", "ble_on", "clock_on", "tz", "bat_pct",
]);
const boolFields = new Set(["led_on", "led_feedback", "button_sound", "rotate", "ble_on", "clock_on", "bat_pct"]);
const stringFields = new Set(["theme", "led_mode", "led_pixels", "tz"]);
const hexColor = /^#[0-9a-fA-F]{6}$/;
export const maxDuration = 10;

function secret(request: Request) {
  return request.headers.get("authorization")?.match(/^Bearer ([0-9a-f]{64})$/i)?.[1] ?? "";
}

function noStore(body: unknown, status = 200) {
  return Response.json(body, { status, headers: { "Cache-Control": "no-store", "Access-Control-Allow-Origin": "*" } });
}

export function OPTIONS() {
  return new Response(null, { status: 204, headers: {
    "Access-Control-Allow-Origin": "*", "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
    "Access-Control-Allow-Headers": "Authorization, Content-Type", "Access-Control-Max-Age": "600",
  } });
}

export async function GET(request: Request, ctx: RouteContext<"/api/board-control/[id]">) {
  const { id } = await ctx.params;
  const requestId = new URL(request.url).searchParams.get("requestId");
  if (requestId) {
    if (!/^[0-9a-f]{16}$/.test(requestId)) return noStore({ error: "Invalid request ID." }, 400);
    const item = await controlRequestStatus(id, secret(request), requestId);
    return item ? controlResponse(item) : noStore({ error: "Request not found or API key invalid." }, 404);
  }
  const device = await controlDevice(id, secret(request));
  if (!device) return noStore({ error: "Invalid API key or project inactive." }, 401);
  const settings = Object.fromEntries([...fields].map((field) => [field, device.settings[field]]));
  const pixels = parsePixels(device.settings.led_pixels, device.settings.led_color);
  const ledRgb = pixels.every((pixel) => pixel.color === pixels[0].color) ? `#${pixels[0].color}` : null;
  return noStore({ success: true, message: "Board state retrieved from the console.", device: { id: device.id, name: device.name, online: device.online, syncing: device.syncing },
    settings: { ...settings, led_rgb: ledRgb, led_pixels_rgb: pixels.map((pixel) => ({
      r: parseInt(pixel.color.slice(0, 2), 16), g: parseInt(pixel.color.slice(2, 4), 16), b: parseInt(pixel.color.slice(4, 6), 16),
      level: pixel.level, blink: pixel.blink,
    })) },
  });
}

export async function POST(request: Request, ctx: RouteContext<"/api/board-control/[id]">) {
  const { id } = await ctx.params;
  const key = secret(request);
  const device = await controlDevice(id, key);
  if (!device) return noStore({ error: "Invalid API key or project inactive." }, 401);
  if (device.projectApi < 8 || device.activeProjectVersion.localeCompare("1.0.5", undefined, { numeric: true }) < 0)
    return noStore({ error: "Update base firmware to v1.1.27 and Board control API to v1.0.5 for confirmed requests." }, 409);
  const body = await request.json().catch(() => null);
  if (!body || typeof body !== "object" || Array.isArray(body) || (!body.settings && !body.audio)) return noStore({ error: "JSON settings or audio required." }, 400);

  let settings: Partial<DeviceSettings> | undefined;
  if (body.settings !== undefined) {
    if (!body.settings || typeof body.settings !== "object" || Array.isArray(body.settings)) return noStore({ error: "settings must be an object." }, 400);
    const raw = { ...(body.settings as Record<string, unknown>) };
    if (!Object.keys(raw).length) return noStore({ error: "settings is empty." }, 400);
    if (raw.led_rgb !== undefined) {
      if (typeof raw.led_rgb !== "string" || !hexColor.test(raw.led_rgb) || raw.led_pixels !== undefined || raw.led_pixels_rgb !== undefined) {
        return noStore({ error: "led_rgb must be #RRGGBB and cannot be combined with led_pixels." }, 400);
      }
      raw.led_pixels = Array(LED_COUNT).fill(`${raw.led_rgb.slice(1).toLowerCase()}:100:0`).join(",");
      delete raw.led_rgb;
    }
    if (raw.led_pixels_rgb !== undefined) {
      if (raw.led_pixels !== undefined || !Array.isArray(raw.led_pixels_rgb) || raw.led_pixels_rgb.length !== LED_COUNT) {
        return noStore({ error: "led_pixels_rgb requires exactly 8 LEDs and cannot be combined with led_pixels." }, 400);
      }
      raw.led_pixels = raw.led_pixels_rgb;
      delete raw.led_pixels_rgb;
    }
    if (Array.isArray(raw.led_pixels)) {
      if (raw.led_pixels.length !== LED_COUNT || !raw.led_pixels.every((pixel) => {
        if (!pixel || typeof pixel !== "object" || Array.isArray(pixel)) return false;
        return [pixel.r, pixel.g, pixel.b].every((value) => Number.isInteger(value) && value >= 0 && value <= 255) &&
          (pixel.level === undefined || Number.isInteger(pixel.level) && pixel.level >= 0 && pixel.level <= 100) &&
          (pixel.blink === undefined || typeof pixel.blink === "boolean");
      })) return noStore({ error: "led_pixels must contain 8 RGB objects with r/g/b 0–255, optional level 0–100 and blink boolean." }, 400);
      raw.led_pixels = (raw.led_pixels as Array<{ r: number; g: number; b: number; level?: number; blink?: boolean }>).map((pixel) =>
        `${[pixel.r, pixel.g, pixel.b].map((value) => value.toString(16).padStart(2, "0")).join("")}:${pixel.level ?? 100}:${pixel.blink ? 1 : 0}`).join(",");
    }
    for (const [field, value] of Object.entries(raw)) {
      if (!fields.has(field as keyof DeviceSettings)) return noStore({ error: `Unknown setting: ${field}` }, 400);
      const valid = stringFields.has(field) ? typeof value === "string"
        : boolFields.has(field) ? typeof value === "boolean"
        : typeof value === "number" && Number.isInteger(value);
      if (!valid) return noStore({ error: `Invalid value for ${field}.` }, 400);
    }
    const sanitized = sanitizeSettings(raw, device.settings);
    settings = {};
    for (const [field, value] of Object.entries(raw)) {
      if (sanitized[field as keyof DeviceSettings] !== value) return noStore({ error: `Invalid value for ${field}.` }, 400);
      Object.assign(settings, { [field]: value });
    }
  }

  let audio: ControlAudio | undefined;
  let upload: Buffer | undefined;
  if (body.audio !== undefined) {
    const raw = body.audio;
    if (!raw || typeof raw !== "object" || Array.isArray(raw)) return noStore({ error: "audio must be an object." }, 400);
    if (raw.type === "stop" || raw.type === "pause" || raw.type === "play") audio = { type: raw.type, value: "" };
    else if (raw.type === "beep") {
      if (!Number.isInteger(raw.frequency) || raw.frequency < 100 || raw.frequency > 4000 ||
          !Number.isInteger(raw.durationMs) || raw.durationMs < 20 || raw.durationMs > 3000) {
        return noStore({ error: "Beep requires frequency 100–4000 Hz and durationMs 20–3000." }, 400);
      }
      audio = { type: "beep", value: `${raw.frequency},${raw.durationMs}` };
    } else if (raw.type === "music") {
      if (typeof raw.data === "string") {
        if (raw.data.length > 2_800_000 || !/^[A-Za-z0-9+/]+={0,2}$/.test(raw.data)) return noStore({ error: "Invalid base64 MP3 data (maximum 2 MB)." }, 400);
        upload = Buffer.from(raw.data, "base64");
        const mp3Header = upload.subarray(0, 3).toString("ascii") === "ID3" ||
          (upload[0] === 0xff && (upload[1] & 0xe0) === 0xe0);
        if (!upload.length || upload.length > 2 * 1024 * 1024 || !mp3Header) {
          return noStore({ error: "MP3 data must be a valid base64 MP3 file of at most 2 MB." }, 400);
        }
      } else {
        if (typeof raw.url !== "string" || raw.url.length > 240 || /[|\x00-\x1f\x7f]/.test(raw.url) ||
            !/^https?:\/\//.test(raw.url)) return noStore({ error: "Music needs an HTTP(S) MP3 stream URL or base64 MP3 data." }, 400);
        audio = { type: "music", value: raw.url };
      }
    } else return noStore({ error: "audio.type must be beep, music, pause, play or stop." }, 400);
  }
  let fileId = "";
  if (upload) {
    fileId = randomBytes(32).toString("hex");
    await db.deviceFile.deleteMany({ where: { deviceId: id, createdAt: { lt: new Date(Date.now() - 30 * 60_000) } } });
    await db.deviceFile.create({ data: { id: fileId, deviceId: id, bytes: Uint8Array.from(upload) } });
    audio = { type: "clip", value: new URL(`/api/board-control/${id}/audio/${fileId}`, request.url).toString() };
    if (audio.value.length > 240) {
      await db.deviceFile.delete({ where: { id: fileId } });
      return noStore({ error: "Server URL is too long for the board." }, 400);
    }
  }
  let updated;
  try {
    updated = await updateControlDevice(id, key, settings, audio);
  } catch (error) {
    if (fileId) await db.deviceFile.delete({ where: { id: fileId } });
    if (error instanceof Error && /^(No music to |Uploaded MP3 expired|Control request queue is full)/.test(error.message)) return noStore({ error: error.message }, 409);
    throw error;
  }
  if (!updated) {
    if (fileId) await db.deviceFile.delete({ where: { id: fileId } });
    return noStore({ error: "API key changed or project inactive." }, 401);
  }
  const deadline = Date.now() + 6500;
  let completed = updated;
  while (Date.now() < deadline && !request.signal.aborted) {
    await new Promise((resolve) => setTimeout(resolve, 400));
    completed = await controlRequestStatus(id, key, updated.id) ?? completed;
    if (completed.status === "done" || completed.status === "failed") break;
  }
  return controlResponse(completed);
}

function controlResponse(item: ControlRequest) {
  const done = item.status === "done";
  const failed = item.status === "failed";
  return noStore({ success: done, status: item.status, requestId: item.id,
    message: done ? (item.result || "Board confirmed the request completed successfully.")
      : failed ? (item.result || "The board could not complete the request.")
        : item.status === "sent" ? "Request sent to the board; waiting for its completion acknowledgment."
          : "Request is queued and waiting for the board.",
  }, done ? 200 : failed ? 409 : 202);
}
