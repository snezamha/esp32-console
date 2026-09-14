import { getUser, unauthorized } from "@/lib/auth";
import { BOARDS } from "@/lib/boards";
import { listDevices, queueCommand } from "@/lib/device-store";
import type { CommandType } from "@/lib/device-types";

const TEST_KEYS = ["all", "battery", "memory", "buttons", "led", "sd", "codec", "mic", "speaker", "wifi", "ble"];

/**
 * Sends a command to the device.
 * Body: { type, text? (notify), test? (test), version? (ota), ssid?, password? (wifi_add) }
 */
export async function POST(request: Request, ctx: RouteContext<"/api/devices/[id]/commands">) {
  const user = await getUser();
  if (!user) return unauthorized();
  const { id } = await ctx.params;
  const body = await request.json().catch(() => null);
  const device = (await listDevices(user.id)).find((d) => d.id === id);
  if (!device) return Response.json({ error: "Device not found." }, { status: 404 });

  const bad = (error: string) => Response.json({ error }, { status: 400 });
  const type = body?.type as CommandType;
  let arg = "";
  let otaVersion = "";

  switch (type) {
    case "restart":
    case "poweroff":
    case "identify":
    case "wifi_forget":
      break;
    case "notify": {
      // The board font is ASCII only.
      const text = typeof body.text === "string" ? body.text.replace(/[^\x20-\x7e]/g, "").trim().slice(0, 60) : "";
      if (!text) return bad("Enter a message (English letters, digits and punctuation).");
      arg = text;
      break;
    }
    case "test":
      if (!TEST_KEYS.includes(body.test)) return bad("Unknown test.");
      arg = body.test;
      break;
    case "ota": {
      const board = BOARDS.find((b) => b.id === device.board);
      const version = board?.versions.find((v) => v.version === body.version);
      if (!board || !version?.app) return bad("This version has no OTA image. Rebuild it with pnpm firmware:build.");
      arg = new URLSearchParams({
        path: `/firmware/${board.id}/${version.version}.app.bin`,
        version: version.version,
        size: String(version.app.size),
        md5: version.app.md5,
      }).toString();
      otaVersion = version.version;
      break;
    }
    case "wifi_add": {
      const ssid = typeof body.ssid === "string" ? body.ssid : "";
      const password = typeof body.password === "string" ? body.password : "";
      if (!ssid || ssid.length > 32) return bad("Network name must be 1–32 characters.");
      if (password && (password.length < 8 || password.length > 63)) return bad("Password must be 8–63 characters.");
      arg = new URLSearchParams({ ssid, password }).toString();
      break;
    }
    default:
      return bad("Unknown command.");
  }

  const updated = await queueCommand(user.id, id, type, arg, otaVersion);
  if (!updated) return Response.json({ error: "Device not found." }, { status: 404 });
  return Response.json({ device: updated }, { status: 202 });
}
