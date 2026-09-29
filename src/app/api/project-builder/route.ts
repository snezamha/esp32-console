import { getUser, unauthorized } from "@/lib/auth";
import { allowlistConfigured } from "@/lib/allowlist";
import { builderHeader, BuilderError, compileProject } from "@/lib/project-builder";
import { withinLimit } from "@/lib/rate-limit";

export const runtime = "nodejs";
/** Builds per user per hour. Each one runs a compiler on the server, so it is capped even for a
 * trusted account. */
const MAX_BUILDS_PER_HOUR = 60;
let activeBuilds = 0;

/**
 * Compiling submitted C on the server is only offered to accounts this deployment names. Without
 * `AUTH_ALLOWED_EMAILS` any Google account can sign in, and the compiler must not be part of that.
 */
const BUILDER_CLOSED_REASON =
  "Project Builder is disabled until AUTH_ALLOWED_EMAILS lists the accounts allowed to use this console.";

function builderClosed() {
  return allowlistConfigured() ? null : Response.json({ error: BUILDER_CLOSED_REASON }, { status: 403 });
}

export async function GET(request: Request) {
  const user = await getUser();
  if (!user) return unauthorized();
  // `?status=1` lets the editor disable compiling up front instead of failing on the first click.
  if (new URL(request.url).searchParams.has("status")) {
    const closed = builderClosed();
    return Response.json(
      { enabled: !closed, reason: closed ? BUILDER_CLOSED_REASON : "" },
      { headers: { "Cache-Control": "no-store" } },
    );
  }
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
  const closed = builderClosed();
  if (closed) return closed;
  if (!(await withinLimit(`build:${user.id}`, MAX_BUILDS_PER_HOUR, 60 * 60 * 1000))) {
    return Response.json({ error: "Too many builds this hour. Try again later." }, { status: 429 });
  }
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
