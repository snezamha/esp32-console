import type { DeviceCommand } from "@/lib/device-types";
export type ProjectPhase = "queued" | "sent" | "connecting" | "downloading" | "verifying" | "writing" | "activating" | "done" | "failed" | "cancelled";
export type ProjectLog = { seq: number; at: number; level: "info" | "error"; message: string };
export type ProjectTransfer = { phase: ProjectPhase; progress: number; bytes: number; total: number; logs: ProjectLog[]; name: string; version: string };
export function projectPending(command?: DeviceCommand) {
  return !!command && (command.status === "queued" || command.status === "sent");
}
export function expireProjectCommands(commands: DeviceCommand[], now = Date.now()): DeviceCommand[] {
  return commands.map((command) => {
    if (!command.type.startsWith("project_") || !projectPending(command)) return command;
    const timeout = command.status === "queued" ? 10 * 60_000 : 120_000;
    if (now - command.updatedAt < timeout) return command;
    const result = command.status === "queued" ? "The board did not come online before the request expired." : "The board stopped reporting installation progress. It may have restarted or lost power. Reconnect the board and retry.";
    return { ...command, status: "failed", result, updatedAt: now, transfer: command.transfer ? { ...command.transfer, phase: "failed", logs: [...command.transfer.logs, { seq: -now, at: now, level: "error" as const, message: result }].slice(-200) } : undefined };
  });
}
