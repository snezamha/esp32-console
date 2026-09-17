import { getUser, unauthorized } from "@/lib/auth";
import { controlKeyEnabled, revokeControlKey, rotateControlKey } from "@/lib/device-store";

export async function GET(_request: Request, ctx: RouteContext<"/api/devices/[id]/control-key">) {
  const user = await getUser();
  if (!user) return unauthorized();
  const { id } = await ctx.params;
  const enabled = await controlKeyEnabled(user.id, id);
  return enabled === null ? Response.json({ error: "Device not found." }, { status: 404 }) : Response.json({ enabled });
}

export async function POST(_request: Request, ctx: RouteContext<"/api/devices/[id]/control-key">) {
  const user = await getUser();
  if (!user) return unauthorized();
  const { id } = await ctx.params;
  const key = await rotateControlKey(user.id, id);
  if (!key) return Response.json({ error: "Load Board control API on this device first." }, { status: 409 });
  return Response.json({ key }, { headers: { "Cache-Control": "no-store" } });
}

export async function DELETE(_request: Request, ctx: RouteContext<"/api/devices/[id]/control-key">) {
  const user = await getUser();
  if (!user) return unauthorized();
  const { id } = await ctx.params;
  return await revokeControlKey(user.id, id) ? new Response(null, { status: 204 }) : Response.json({ error: "Device not found." }, { status: 404 });
}
