import { projectFileForBoard } from "@/lib/device-store";
export async function GET(request: Request, ctx: RouteContext<"/api/devices/[id]/projects/[commandId]/file">) {
  const { id, commandId } = await ctx.params;
  const token = request.headers.get("authorization")?.replace(/^Bearer /, "") ?? "";
  if (!token) return new Response(null, { status: 401 });
  const bytes = await projectFileForBoard(id, commandId, token);
  return bytes ? new Response(new Uint8Array(bytes), { headers: { "Content-Type": "application/octet-stream", "Content-Length": String(bytes.length), "Cache-Control": "no-store" } }) : new Response(null, { status: 404 });
}
