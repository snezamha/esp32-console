import { getUser, unauthorized } from "@/lib/auth";
import { clearLimit, withinLimit } from "@/lib/rate-limit";
import { claimCode, listDevices } from "@/lib/device-store";

const CLAIM_WINDOW_MS = 10 * 60 * 1000;
/** Claim attempts per user, to stop guessing the 6-digit codes. A successful claim clears the
 * counter, so only a run of wrong codes reaches the limit. */
const MAX_CLAIMS = 10;
/**
 * Claim attempts across every account in the same window. A code is looked up across all boards,
 * so without this a pool of accounts could search the 6-digit space for a stranger's pending board.
 */
const MAX_CLAIMS_GLOBAL = 60;
const GLOBAL_CLAIM_KEY = "claim:attempts:all";

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
  const relinkId = typeof body?.relinkId === "string" ? body.relinkId : undefined;
  if (!/^\d{6}$/.test(code)) {
    return Response.json({ error: "Enter the 6-digit code shown on the device." }, { status: 400 });
  }

  const userKey = `claim:attempts:${user.id}`;
  const tooMany = Response.json({ error: "Too many wrong codes. Try again in a few minutes." }, { status: 429 });
  // Counted before the lookup, so an attempt that throws still costs the caller a try.
  if (!(await withinLimit(userKey, MAX_CLAIMS, CLAIM_WINDOW_MS))) return tooMany;
  if (!(await withinLimit(GLOBAL_CLAIM_KEY, MAX_CLAIMS_GLOBAL, CLAIM_WINDOW_MS))) return tooMany;

  let device;
  try {
    device = await claimCode(user.id, code, relinkId);
  } catch (error) {
    return Response.json({ error: error instanceof Error ? error.message : "Could not reconnect this device." }, { status: 409 });
  }
  if (!device) {
    return Response.json(
      { error: "No device is showing this code. Check the screen or restart the device for a new code." },
      { status: 404 },
    );
  }
  await clearLimit(userKey);
  return Response.json({ device }, { status: 201 });
}
