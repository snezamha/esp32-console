import { getUser, unauthorized } from "@/lib/auth";
import { builderHeader, BuilderError, compileProject } from "@/lib/project-builder";

export const runtime = "nodejs";
let activeBuilds = 0;

export async function GET() {
  const user = await getUser();
  if (!user) return unauthorized();
  return new Response(await builderHeader(), {
    headers: {
      "Content-Type": "text/x-c; charset=utf-8",
      "Content-Disposition": 'attachment; filename="project_builder.h"',
      "Cache-Control": "no-store",
    },
  });
}

export async function POST(request: Request) {
  const user = await getUser();
  if (!user) return unauthorized();
  if (activeBuilds >= 2) return Response.json({ error: "The compiler is busy. Try again in a moment." }, { status: 429 });
  activeBuilds += 1;
  try {
    if (Number(request.headers.get("content-length")) > 52 * 1024) throw new BuilderError("Build request is too large.", 413);
    const payload = await request.text();
    if (Buffer.byteLength(payload) > 52 * 1024) throw new BuilderError("Build request is too large.", 413);
    const body = (() => { try { return JSON.parse(payload); } catch { return null; } })();
    if (!body || typeof body !== "object") throw new BuilderError("Invalid build request.", 400);
    const result = await compileProject({
      id: typeof body.id === "string" ? body.id : "",
      name: typeof body.name === "string" ? body.name : "",
      version: typeof body.version === "string" ? body.version : "",
      description: typeof body.description === "string" ? body.description : "",
      source: typeof body.source === "string" ? body.source : "",
    });
    return new Response(result.bytes, {
      headers: {
        "Content-Type": "application/x-elf",
        "Content-Disposition": `attachment; filename="${result.metadata.id}-${result.metadata.version}.elf"`,
        "Cache-Control": "no-store",
      },
    });
  } catch (error) {
    const status = error instanceof BuilderError ? error.status : 500;
    return Response.json({ error: error instanceof Error ? error.message : "Build failed." }, { status });
  } finally {
    activeBuilds -= 1;
  }
}
