import { db } from "@/lib/db";

/** The random file ID is a short-lived bearer URL used only by the ESP32 MP3 decoder. */
export async function GET(_request: Request, ctx: RouteContext<"/api/board-control/[id]/audio/[file]">) {
  const { id, file } = await ctx.params;
  if (!/^[0-9a-f]{64}$/.test(file)) return new Response(null, { status: 404 });
  const stored = await db.deviceFile.findFirst({
    where: { id: file, deviceId: id, createdAt: { gt: new Date(Date.now() - 30 * 60_000) } },
    select: { bytes: true },
  });
  if (!stored) return new Response(null, { status: 404 });
  const bytes = Buffer.from(stored.bytes);
  return new Response(new Uint8Array(bytes), { headers: {
    "Content-Type": "audio/mpeg", "Content-Length": String(bytes.length), "Cache-Control": "private, no-store",
  } });
}
