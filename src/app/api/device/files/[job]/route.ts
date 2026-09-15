import { MAX_DEVICE_FILE_BYTES, storeBoardTransfer, uploadForBoard } from "@/lib/device-store";

const bearer = (request: Request) => request.headers.get("authorization")?.replace(/^Bearer /, "") ?? "";

/** Board: fetches a browser upload for an sd_upload operation. */
export async function GET(request: Request, ctx: RouteContext<"/api/device/files/[job]">) {
  const { job } = await ctx.params;
  const bytes = await uploadForBoard(bearer(request), job);
  if (!bytes) return new Response(null, { status: 404 });
  return new Response(new Uint8Array(bytes), { headers: { "Content-Type": "application/octet-stream", "Content-Length": String(bytes.length), "Cache-Control": "no-store" } });
}

/** Board: delivers a folder listing (sd_list) or file contents (sd_download). */
export async function POST(request: Request, ctx: RouteContext<"/api/device/files/[job]">) {
  const { job } = await ctx.params;
  if (Number(request.headers.get("content-length")) > MAX_DEVICE_FILE_BYTES) return new Response(null, { status: 413 });
  const bytes = Buffer.from(await request.arrayBuffer());
  return new Response(null, { status: (await storeBoardTransfer(bearer(request), job, bytes)) ? 200 : 404 });
}
