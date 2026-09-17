import { DEFAULT_SETTINGS, formatSettingValue, sanitizeSettings } from "@/lib/device-settings";
import { controlRequestForBoard, projectSettingsForBoard, syncBoard, type SyncResult } from "@/lib/device-store";
import { weatherPayload } from "@/lib/weather";
import type { ProjectPhase } from "@/lib/project-transfers";
import type { TestResult } from "@/lib/device-types";

// Bounded to stay under Vercel's default (Hobby-plan) 10 s function limit; raise both this and
// `maxDuration` together if the project is on a plan with a longer limit.
export const maxDuration = 10;
const MAX_WAIT_S = 8;
const TEST_STATUSES = ["idle", "running", "ok", "fail", "info"];
const OTA_STATES = ["downloading", "done", "failed"];

/**
 * Board check-in, called by the firmware while Wi-Fi is up.
 *
 * Request: `Authorization: Bearer <token>` header once linked (revoke and poll requests alike),
 * plus form fields:
 *   secret      random hex generated at boot (identifies the pairing while unlinked)
 *   code        pairing code the board currently shows
 *   mac, board, fw, ip, rssi, battery, battery_mv, charging, heap, uptime, rev
 *   s.<key>     current value of each setting in DeviceSettings
 *   sd          SD card: <mounted 0|1>|<total bytes>|<free bytes>
 *   sd.job      running SD operation: <command id>|<done>|<total> (bytes, or sectors for a format)
 *   t.<key>     hardware check result: <status>|<detail>
 *   ota         <state>|<progress>|<error>
 *   wifi1, wifi2  saved network names
 *   ack         <command id>|ok|<result> or <command id>|fail|<error>, repeatable
 *   wait=<s>    hold the request until there is something to deliver (long poll)
 *   unlink=1    forget this board (sent with its token)
 *
 * Response: text/plain `key=value` lines, easy to parse without a JSON library:
 *   status=pending   code=123456  expires=<s>
 *   status=linked    token=…  name=…  rev=<n>  [set.<key>=<value> …]  [cmd.<id>.<type>=<url-encoded arg> …]
 *   status=unlinked
 */
export async function POST(request: Request) {
  const startedAt = Date.now();
  let form: FormData;
  try {
    form = await request.formData();
  } catch {
    return reply(["status=error", "error=form body required"], 400);
  }
  const field = (name: string, max = 64) =>
    String(form.get(name) ?? "")
      .slice(0, max)
      .replace(/[\r\n]/g, "");
  const int = (name: string, fallback: number) => {
    const value = parseInt(field(name), 10);
    return Number.isFinite(value) ? value : fallback;
  };
  const bearer = request.headers.get("authorization")?.match(/^Bearer ([0-9a-f]{16,64})$/i)?.[1] ?? "";
  const formToken = field("token"); // Older firmware sent its token in the form body.
  if (bearer && formToken && bearer !== formToken) return reply(["status=error", "error=conflicting token"], 400);
  const token = bearer || formToken;

  const reported: Record<string, unknown> = {};
  for (const key of Object.keys(DEFAULT_SETTINGS)) {
    const value = form.get(`s.${key}`);
    if (value !== null) reported[key] = String(value);
  }

  const tests: Record<string, TestResult> = {};
  for (const [name, value] of form.entries()) {
    if (!name.startsWith("t.") || typeof value !== "string") continue;
    const [status, ...detail] = value.split("|");
    if (TEST_STATUSES.includes(status)) {
      tests[name.slice(2, 24)] = { status: status as TestResult["status"], detail: detail.join("|").slice(0, 64) };
    }
  }

  const [otaState, otaProgress, ...otaError] = field("ota", 120).split("|");
  const acks = form
    .getAll("ack")
    .map(String)
    .map((value) => {
      const [id, outcome, ...result] = value.split("|");
      return { id: id.slice(0, 16), ok: outcome === "ok", result: result.join("|").slice(0, 200) };
    })
    .filter((ack) => ack.id);

  const [projectId, projectPhase, projectProgress, projectBytes, projectTotal] = field("p.status", 160).split("|");
  const [controlId, controlOutcome, ...controlResult] = field("control_ack", 180).split("|");
  const phases = ["connecting", "downloading", "verifying", "writing", "activating", "done", "failed", "cancelled"];
  const projectLogs = form.getAll("p.log").slice(-40).map(String).map((line) => {
    const [id, seq, level, ...message] = line.split("|");
    return { id: id.slice(0, 16), seq: parseInt(seq, 10), level: level === "error" ? "error" as const : "info" as const, message: message.join("|").slice(0, 240) };
  }).filter((log) => log.id && Number.isFinite(log.seq));
  const report = {
    secret: field("secret").toLowerCase(),
    token,
    code: field("code", 6),
    mac: field("mac", 17).toUpperCase(),
    board: field("board"),
    firmware: field("fw", 16),
    ip: field("ip", 45),
    rssi: int("rssi", 0),
    battery: int("battery", -1),
    batteryMv: Math.max(0, Math.min(5000, int("battery_mv", 0))),
    charging: field("charging") === "1",
    heap: int("heap", 0),
    uptime: int("uptime", 0),
    rev: int("rev", 0),
    settings: Object.keys(reported).length ? sanitizeSettings(reported, DEFAULT_SETTINGS) : null,
    projectApi: Math.max(0, Math.min(9, int("project_api", 0))),
    projectVersion: field("project_version", 32),
    projectSha256: field("project_sha256", 64).toLowerCase(),
    projectSafeMode: field("project_safe") === "1",
    sdCard: sdCard(field("sd", 64)),
    projectStatus: projectId && phases.includes(projectPhase) ? { id: projectId, phase: projectPhase as ProjectPhase, progress: Math.min(100, Math.max(0, parseInt(projectProgress, 10) || 0)), bytes: Math.max(0, parseInt(projectBytes, 10) || 0), total: Math.max(0, parseInt(projectTotal, 10) || 0) } : null,
    projectLogs,
    fileProgress: fileProgress(field("sd.job", 64)),
    resetReason: field("reset_reason"),
    tests,
    ota: OTA_STATES.includes(otaState)
      ? {
          state: otaState as "downloading" | "done" | "failed",
          progress: Math.min(100, Math.max(0, parseInt(otaProgress, 10) || 0)),
          error: otaError.join("|"),
        }
      : null,
    networks: form.has("wifi1") ? [field("wifi1", 32), field("wifi2", 32)] : null,
    acks,
    controlAck: /^[0-9a-f]{16}$/.test(controlId) && ["ok", "fail"].includes(controlOutcome)
      ? { id: controlId, ok: controlOutcome === "ok", result: controlResult.join("|").slice(0, 120) } : null,
    unlink: field("unlink") === "1",
  };

  const waitS = Math.min(report.projectStatus && !["done", "failed", "cancelled"].includes(report.projectStatus.phase) ? 0 : report.settings?.project === "weather" ? 4 : MAX_WAIT_S, Math.max(0, int("wait", 0)));
  const result = await syncBoard(report, waitS * 1000, request.signal);
  if (request.signal.aborted) return reply([], 499);
  const lines = render(result);
  if (result.status === "linked" && report.settings) {
    const settings = { ...report.settings, ...result.settings };
    if (settings.project === "analog-clock") {
      const config = await projectSettingsForBoard(report.token, "analog-clock");
      if (config) lines.push(`project_data=${encodeURIComponent(`${config.seconds ? "1" : "0"}|${config.date ? "1" : "0"}|${config.style ?? "ticks"}`)}`);
    }
    if (settings.project === "weather") {
      const budget = Math.max(1, Math.min(4000, 8500 - (Date.now() - startedAt)));
      // Coordinates are console-side project configuration, not base firmware settings.
      const config = await projectSettingsForBoard(report.token, "weather");
      if (config) lines.push(`project_data=${encodeURIComponent(await weatherPayload(config, budget))}`);
    }
    if (settings.project === "radio") {
      const config = await projectSettingsForBoard(report.token, "radio");
      if (config && typeof config.station === "string") {
        // data[0] is the starting station id; data[1] is 3 slots of "url|name|lang", pipe-joined
        // (firmware only splits the payload on the first "|", so the project itself splits data[1]
        // further, 9 fields total). An unset slot stays three empty strings, not omitted, so the
        // firmware can tell "second custom station unset" from "third one shifted up".
        const text = (key: string) => (typeof config[key] === "string" ? (config[key] as string) : "");
        const custom = [1, 2, 3]
          .map((n) => [text(`custom${n}`), text(`custom${n}_name`), text(`custom${n}_lang`) || "EN"].join("|"))
          .join("|");
        lines.push(`project_data=${encodeURIComponent(`${config.station}|${custom}`)}`);
      }
    }
    if (settings.project === "board-control-api") {
      const control = await controlRequestForBoard(report.token);
      if (control) lines.push(`project_data=${encodeURIComponent(`${control.id}|${control.audio?.type ?? "settings"}|${control.audio?.value ?? ""}`)}`);
    }
  }
  return reply(lines);
}

/** `mounted|total bytes|free bytes`, sent by firmware with display ABI 3. */
function fileProgress(value: string) {
  const [id, done, total] = value.split("|");
  if (!id || !/^[0-9a-f]{1,16}$/.test(id)) return null;
  const count = (text: string) => Math.max(0, Number.parseInt(text, 10) || 0);
  return { id, done: Math.min(count(done), count(total)), total: count(total) };
}

function sdCard(value: string) {
  const [mounted, total, free] = value.split("|");
  if (!value || !["0", "1"].includes(mounted)) return null;
  const bytes = (text: string) => Math.max(0, Number.parseInt(text, 10) || 0);
  return { mounted: mounted === "1", total: bytes(total), free: Math.min(bytes(free), bytes(total)) };
}

function render(result: SyncResult) {
  switch (result.status) {
    case "pending":
      return ["status=pending", `code=${result.code}`, `expires=${result.expiresIn}`];
    case "unlinked":
      return ["status=unlinked"];
    case "linked":
      return [
        "status=linked",
        `token=${result.token}`,
        `name=${result.name.replace(/[\r\n]/g, " ")}`,
        `rev=${result.rev}`,
        ...Object.entries(result.settings).map(([key, value]) => `set.${key}=${formatSettingValue(value)}`),
        ...result.commands.map((c) => `cmd.${c.id}.${c.type}=${encodeURIComponent(c.arg)}`),
      ];
  }
}

function reply(lines: string[], status = 200) {
  return new Response(lines.length ? lines.join("\n") + "\n" : null, {
    status,
    headers: { "Content-Type": "text/plain; charset=utf-8", "Cache-Control": "no-store" },
  });
}
