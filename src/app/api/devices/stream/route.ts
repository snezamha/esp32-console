import { getUser, unauthorized } from "@/lib/auth";
import { deviceRowsSignature, listDevices, waitForChange } from "@/lib/device-store";

// Kept short and single-shot (one poll cycle per response) rather than a long-lived stream, so it
// stays well under Vercel's default (Hobby-plan) 10 s function limit; the browser's EventSource
// reconnects automatically, so the effect is still a live feed, just chunked into short requests.
export const maxDuration = 10;
const WAIT_MS = 8000;

/** Server-sent events: one `devices` event per request, holding briefly for a change. */
export async function GET(request: Request) {
  const user = await getUser();
  if (!user) return unauthorized();

  const encoder = new TextEncoder();
  const stream = new ReadableStream<Uint8Array>({
    async start(controller) {
      const send = (devices: Awaited<ReturnType<typeof listDevices>>) => {
        controller.enqueue(encoder.encode(`event: devices\ndata: ${JSON.stringify(devices)}\n\n`));
      };
      try {
        const initial = await listDevices(user.id);
        send(initial);
        if (!request.signal.aborted) {
          const changed = await waitForChange(user.id, await deviceRowsSignature(user.id), WAIT_MS, request.signal);
          if (!request.signal.aborted) send(changed);
        }
      } catch {
        // Client disconnected or the database hiccuped; the next EventSource retry recovers.
      } finally {
        try {
          controller.close();
        } catch {
          // Already closed.
        }
      }
    },
  });

  return new Response(stream, {
    headers: {
      "Content-Type": "text/event-stream; charset=utf-8",
      "Cache-Control": "no-store, no-transform",
      Connection: "keep-alive",
      "X-Accel-Buffering": "no",
    },
  });
}
