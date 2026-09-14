import { getUser, unauthorized } from "@/lib/auth";
import { DEFAULT_SETTINGS, sanitizeSettings } from "@/lib/device-settings";
import { listDevices, removeDevice, updateDevice } from "@/lib/device-store";

/** Renames the device and/or changes its settings. Only settings that differ are sent to the board. */
export async function PATCH(request: Request, ctx: RouteContext<"/api/devices/[id]">) {
  const user = await getUser();
  if (!user) return unauthorized();
  const { id } = await ctx.params;
  const body = await request.json().catch(() => null);
  if (!body || typeof body !== "object") return Response.json({ error: "JSON body required." }, { status: 400 });

  const current = (await listDevices(user.id)).find((d) => d.id === id);
  if (!current) return Response.json({ error: "Device not found." }, { status: 404 });

  const name = typeof body.name === "string" ? body.name.trim().slice(0, 40) : undefined;
  const settings =
    body.settings && typeof body.settings === "object"
      ? sanitizeSettings(body.settings, { ...DEFAULT_SETTINGS, ...current.settings })
      : undefined;

  const device = await updateDevice(user.id, id, { name, settings });
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
