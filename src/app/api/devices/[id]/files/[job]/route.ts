import { getUser, unauthorized } from "@/lib/auth";
import { fileJob } from "@/lib/device-store";

/** Result of one SD card operation. `?content=1` downloads the file of a finished sd_download. */
export async function GET(request: Request, ctx: RouteContext<"/api/devices/[id]/files/[job]">) {
  const user = await getUser();
  if (!user) return unauthorized();
  const { id, job } = await ctx.params;
  const found = await fileJob(user.id, id, job);
  if (!found) return Response.json({ error: "Operation not found." }, { status: 404 });
  if (new URL(request.url).searchParams.has("content")) {
    if (found.job.type !== "sd_download" || !found.bytes) return Response.json({ error: "File is not ready." }, { status: 404 });
    const name = found.path.split("/").pop() || "download";
    return new Response(new Uint8Array(found.bytes), {
      headers: {
        "Content-Type": "application/octet-stream",
        "Content-Length": String(found.bytes.length),
        "Content-Disposition": `attachment; filename*=UTF-8''${encodeURIComponent(name)}`,
        "Cache-Control": "no-store",
      },
    });
  }
  return Response.json({ job: found.job });
}
