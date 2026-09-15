import { getUser, unauthorized } from "@/lib/auth";
import { BOARDS } from "@/lib/boards";
import { listDevices, queueCommand, stopProject, retryProjectFile } from "@/lib/device-store";
import { projectPackage } from "@/lib/projects";
import { isOtaActive } from "@/lib/device-client";
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
  let projectInfo: { name: string; version: string; size: number } | undefined;

  switch (type) {
    case "project_stop": {
      if (typeof body.command !== "string") return bad("Choose an installation to stop.");
      const updated = await stopProject(user.id, id, body.command);
      return updated ? Response.json({ device: updated }) : Response.json({ error: "Device not found." }, { status: 404 });
    }
    case "project_install": {
      if (body.project !== "none" && device.firmware.localeCompare("1.0.9", undefined, { numeric: true }) < 0) return bad("Update the base firmware to v1.0.9 before loading a project.");
      if (typeof body.retry === "string") {
        const previous = device.commands.find((command) => command.id === body.retry && command.type === "project_install");
        if (!previous || previous.status === "queued" || previous.status === "sent") return bad("This installation cannot be retried yet.");
        const values = new URLSearchParams(previous.arg);
        if (values.get("path")?.startsWith("/api/devices/")) {
          try { const updated = await retryProjectFile(user.id, id, body.retry); return Response.json({ device: updated }, { status: 202 }); } catch (error) { return bad(error instanceof Error ? error.message : "Retry failed."); }
        }
        body.project = values.get("id");
      }
      if (!device.projectSupported) return bad("Update the base firmware to v1.0.4 or later.");
      if (isOtaActive(device.ota) || device.commands.some((c) => c.type === "project_install" && c.status === "sent")) return bad("An installation is already running.");
      if (body.project === "none") { arg = new URLSearchParams({ id: "none" }).toString(); break; }
      const project = projectPackage(body.project, device.board);
      if (!project) return bad("Project package is not available for this board.");
      projectInfo = { name: project.name, version: project.version, size: project.size };
      arg = new URLSearchParams({ id: project.id, version: project.version, path: project.path, abi: String(project.abi), size: String(project.size), md5: project.md5, sha256: project.sha256 }).toString();
      break;
    }
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
        sha256: version.app.sha256,
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

  let updated;
  try { updated = await queueCommand(user.id, id, type, arg, otaVersion, projectInfo); }
  catch (error) {
    if (error instanceof Error && error.message === "An installation is already running.") return Response.json({ error: error.message }, { status: 409 });
    throw error;
  }
  if (!updated) return Response.json({ error: "Device not found." }, { status: 404 });
  return Response.json({ device: updated }, { status: 202 });
}
