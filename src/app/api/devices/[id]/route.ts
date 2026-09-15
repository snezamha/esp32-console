import { getUser, unauthorized } from "@/lib/auth";
import { DEFAULT_SETTINGS, sanitizeSettings } from "@/lib/device-settings";
import { listDevices, removeDevice, updateDevice } from "@/lib/device-store";
import { sanitizeProjectConfig } from "@/lib/project-config";

/** Renames the device and/or changes its settings. Only settings that differ are sent to the board. */
export async function PATCH(request: Request, ctx: RouteContext<"/api/devices/[id]">) {
  const user = await getUser();
  if (!user) return unauthorized();
  const { id } = await ctx.params;
  const body = await request.json().catch(() => null);
  if (!body || typeof body !== "object") return Response.json({ error: "JSON body required." }, { status: 400 });

  const current = (await listDevices(user.id)).find((d) => d.id === id);
  if (!current) return Response.json({ error: "Device not found." }, { status: 404 });

  if (body.settings?.project !== undefined && body.settings.project !== current.settings.project) {
    return Response.json({ error: "Load a project package through the project installation command." }, { status: 400 });
  }

  const name = typeof body.name === "string" ? body.name.trim().slice(0, 40) : undefined;
  const settings =
    body.settings && typeof body.settings === "object"
      ? sanitizeSettings(body.settings, { ...DEFAULT_SETTINGS, ...current.settings })
      : undefined;
  let projectSettings;
  if (typeof body.project === "string" && body.projectSettings && typeof body.projectSettings === "object") {
    try {
      projectSettings = {
        project: body.project,
        values: sanitizeProjectConfig(body.project, body.projectSettings, current.projectSettings[body.project]),
      };
    } catch (error) {
      return Response.json({ error: error instanceof Error ? error.message : "Invalid project settings." }, { status: 400 });
    }
  }

  const device = await updateDevice(user.id, id, { name, settings, projectSettings });
  if (!device) return Response.json({ error: "Device not found." }, { status: 404 });
  return Response.json({ device });
}

/** Unlinks the device. The board shows a new code right away if it is online. */
export async function DELETE(_request: Request, ctx: RouteContext<"/api/devices/[id]">) {
  const user = await getUser();
  if (!user) return unauthorized();
  const { id } = await ctx.params;
  if (!(await removeDevice(user.id, id))) {
    return Response.json({ error: "Device not found." }, { status: 404 });
  }
  return new Response(null, { status: 204 });
}
