import { createHash } from "node:crypto";
import { getUser, unauthorized } from "@/lib/auth";
import { listDevices, queueCommand } from "@/lib/device-store";
import { inspectProject } from "@/lib/project-file";
import { isOtaActive } from "@/lib/device-client";
export async function POST(request: Request, ctx: RouteContext<"/api/devices/[id]/projects">) {
  const user = await getUser();
  if (!user) return unauthorized();
  const { id } = await ctx.params;
  const device = (await listDevices(user.id)).find((entry) => entry.id === id);
  if (!device) return Response.json({ error: "Device not found." }, { status: 404 });
  try {
    if (!device.projectSupported || device.firmware.localeCompare("1.0.5", undefined, { numeric: true }) < 0) throw new Error("Update the base firmware to v1.0.5 before uploading a project file.");
    if (isOtaActive(device.ota)) throw new Error("A firmware installation is running.");
    if (Number(request.headers.get("content-length")) > 140 * 1024) throw new Error("Project files must be at most 128 KB.");
    const form = await request.formData();
    const file = form.get("file");
    if (!(file instanceof File) || file.size > 128 * 1024) throw new Error("Choose one project file, at most 128 KB.");
    const bytes = Buffer.from(await file.arrayBuffer());
    const meta = inspectProject(bytes);
    if (meta.board !== device.board) throw new Error("This project targets another board.");
    const arg = new URLSearchParams({ id: meta.id, path: "/api/devices/", abi: "1", size: String(bytes.length), md5: createHash("md5").update(bytes).digest("hex") }).toString();
    const updated = await queueCommand(user.id, id, "project_install", arg, "", { name: meta.name, version: meta.version, size: bytes.length, file: bytes.toString("base64") });
    return Response.json({ device: updated }, { status: 202 });
  } catch (error) { return Response.json({ error: error instanceof Error ? error.message : "Upload failed." }, { status: 400 }); }
}
