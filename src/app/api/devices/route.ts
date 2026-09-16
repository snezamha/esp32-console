import { getUser, unauthorized } from "@/lib/auth";
import { claimCode, listDevices } from "@/lib/device-store";

const MAX_FAILED_CLAIMS = 10;
const FAILED_CLAIM_WINDOW_MS = 10 * 60 * 1000;
/** Wrong codes per user, to stop guessing the 6-digit codes. */
const failedClaims = new Map<string, number[]>();

export async function GET() {
  const user = await getUser();
  if (!user) return unauthorized();
  try {
    return Response.json({ devices: await listDevices(user.id) }, { headers: { "Cache-Control": "no-store" } });
  } catch (error) {
    console.error("Device list unavailable:", error);
    return Response.json({ error: "Devices are unavailable. Check the database connection and schema." }, { status: 503 });
  }
}

/** Adds the board currently showing `code`. */
export async function POST(request: Request) {
  const user = await getUser();
  if (!user) return unauthorized();
  const body = await request.json().catch(() => null);
  const code = typeof body?.code === "string" ? body.code.trim() : "";
  if (!/^\d{6}$/.test(code)) {
    return Response.json({ error: "Enter the 6-digit code shown on the device." }, { status: 400 });
  }

  const now = Date.now();
  const failures = (failedClaims.get(user.id) ?? []).filter((t) => now - t < FAILED_CLAIM_WINDOW_MS);
  if (failures.length >= MAX_FAILED_CLAIMS) {
    return Response.json({ error: "Too many wrong codes. Try again in a few minutes." }, { status: 429 });
  }

  const device = await claimCode(user.id, code);
  if (!device) {
    failedClaims.set(user.id, [...failures, now]);
    return Response.json(
      { error: "No device is showing this code. Check the screen or restart the device for a new code." },
      { status: 404 },
    );
  }
  failedClaims.delete(user.id);
  return Response.json({ device }, { status: 201 });
}
