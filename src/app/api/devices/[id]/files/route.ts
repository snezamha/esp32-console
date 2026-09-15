import { getUser, unauthorized } from "@/lib/auth";
import { compareVersions } from "@/lib/device-client";
import { listDevices, MAX_DEVICE_FILE_BYTES, queueFileCommand } from "@/lib/device-store";
import { SD_FILES_FIRMWARE, type FileCommandType } from "@/lib/device-types";

const JSON_ACTIONS: FileCommandType[] = ["sd_list", "sd_download", "sd_delete", "sd_mkdir", "sd_rename", "sd_format"];

/** Absolute card path without empty, "." or ".." segments (the board checks again). */
function cardPath(value: unknown) {
  if (typeof value !== "string" || !value.startsWith("/") || value.length > 200) return null;
  if (value === "/") return value;
  const segments = value.slice(1).split("/");
  return segments.every((segment) => segment && segment !== "." && segment !== ".." && !/[\x00-\x1f\\]/.test(segment)) ? value : null;
}

/**
 * Starts one SD card operation. JSON body `{ type, path, to? }`, or multipart form `file` + `path`
 * (the destination file path) for an upload. Poll GET /files/<job> for the result.
 */
export async function POST(request: Request, ctx: RouteContext<"/api/devices/[id]/files">) {
  const user = await getUser();
  if (!user) return unauthorized();
  const { id } = await ctx.params;
  const device = (await listDevices(user.id)).find((entry) => entry.id === id);
  if (!device) return Response.json({ error: "Device not found." }, { status: 404 });
  const bad = (error: string, status = 400) => Response.json({ error }, { status });
  if (compareVersions(device.firmware, SD_FILES_FIRMWARE) < 0) return bad(`Update the base firmware to v${SD_FILES_FIRMWARE} to manage the SD card.`);

  let type: FileCommandType;
  const params: Record<string, string> = {};
  let upload: Buffer | undefined;
  if (request.headers.get("content-type")?.startsWith("multipart/form-data")) {
    if (Number(request.headers.get("content-length")) > MAX_DEVICE_FILE_BYTES + 64 * 1024) return bad("Files must be at most 4 MB.", 413);
    const form = await request.formData().catch(() => null);
    const file = form?.get("file");
    const path = cardPath(form?.get("path"));
    if (!(file instanceof File) || file.size > MAX_DEVICE_FILE_BYTES) return bad("Choose one file of at most 4 MB.");
    if (!path || path === "/") return bad("Invalid destination path.");
    type = "sd_upload";
    params.path = path;
    upload = Buffer.from(await file.arrayBuffer());
  } else {
    const body = await request.json().catch(() => null);
    type = body?.type;
    if (!JSON_ACTIONS.includes(type)) return bad("Unknown SD card operation.");
    const path = type === "sd_format" ? "/" : cardPath(body.path);
    if (!path) return bad("Invalid path.");
    if ((type === "sd_delete" || type === "sd_rename" || type === "sd_mkdir") && path === "/") return bad("The card root cannot be changed.");
    params.path = path;
    if (type === "sd_rename") {
      const to = cardPath(body.to);
      if (!to || to === "/") return bad("Invalid new name.");
      params.to = to;
    }
  }
  try {
    const queued = await queueFileCommand(user.id, id, type, params, upload);
    return queued ? Response.json(queued, { status: 202 }) : bad("Device not found.", 404);
  } catch (error) {
    return bad(error instanceof Error ? error.message : "Could not start the operation.", 409);
  }
}
