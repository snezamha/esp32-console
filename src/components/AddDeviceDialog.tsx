"use client";

import { Button, Dialog, DialogBackdrop, DialogPanel, DialogTitle } from "@headlessui/react";
import { useRef, useState, type ClipboardEvent, type KeyboardEvent } from "react";
import { CloseButton, accentButton, secondaryButton } from "@/components/ui";
import type { PublicDevice } from "@/lib/device-types";
import { errorMessage, findKnownPort, loadSerialApi, sendSerialCommand } from "@/lib/esp";

const CODE_LENGTH = 6;
const dialogPanel =
  "w-full max-w-md space-y-5 rounded-2xl bg-white p-5 shadow-xl transition duration-200 ease-out data-closed:translate-y-4 data-closed:opacity-0 dark:bg-zinc-900";

export function AddDeviceDialog({
  open,
  onClose,
  onAdded,
  relinkDevice,
}: {
  open: boolean;
  onClose: () => void;
  onAdded: (device: PublicDevice) => void;
  relinkDevice?: PublicDevice | null;
}) {
  const [digits, setDigits] = useState<string[]>(() => Array(CODE_LENGTH).fill(""));
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const inputs = useRef<(HTMLInputElement | null)[]>([]);
  const code = digits.join("");

  const close = () => {
    onClose();
    setDigits(Array(CODE_LENGTH).fill(""));
    setError(null);
  };

  const fill = (start: number, text: string) => {
    const chars = text.replace(/\D/g, "").slice(0, CODE_LENGTH - start).split("");
    if (!chars.length) return;
    setDigits((prev) => {
      const next = [...prev];
      chars.forEach((c, i) => (next[start + i] = c));
      return next;
    });
    setError(null);
    inputs.current[Math.min(start + chars.length, CODE_LENGTH - 1)]?.focus();
  };

  const onKeyDown = (index: number, event: KeyboardEvent<HTMLInputElement>) => {
    if (event.key === "Backspace" && !digits[index] && index > 0) {
      event.preventDefault();
      setDigits((prev) => prev.map((d, i) => (i === index - 1 ? "" : d)));
      inputs.current[index - 1]?.focus();
    } else if (event.key === "ArrowLeft" && index > 0) {
      inputs.current[index - 1]?.focus();
    } else if (event.key === "ArrowRight" && index < CODE_LENGTH - 1) {
      inputs.current[index + 1]?.focus();
    } else if (event.key === "Enter" && code.length === CODE_LENGTH) {
      void submit();
    }
  };

  const submit = async () => {
    if (code.length !== CODE_LENGTH || busy) return;
    setBusy(true);
    setError(null);
    try {
      const res = await fetch("/api/devices", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ code, ...(relinkDevice ? { relinkId: relinkDevice.id } : {}) }),
      });
      const body = await res.json().catch(() => ({}));
      if (!res.ok) throw new Error(body.error ?? `Could not add device (${res.status})`);
      setDigits(Array(CODE_LENGTH).fill(""));
      onAdded(body.device);
    } catch (err) {
      setError(errorMessage(err));
    } finally {
      setBusy(false);
    }
  };

  return (
    <Dialog open={open} onClose={close} className="relative z-50">
      <DialogBackdrop transition className="fixed inset-0 bg-black/40 transition duration-200 data-closed:opacity-0" />
      <div className="fixed inset-0 flex items-end justify-center p-4 sm:items-center">
        <DialogPanel transition className={dialogPanel}>
          <div className="flex items-start justify-between gap-4">
            <div className="space-y-1">
              <DialogTitle className="text-base font-semibold">{relinkDevice ? `Reconnect ${relinkDevice.name || relinkDevice.mac}` : "Add Device"}</DialogTitle>
              <p className="text-sm text-zinc-500">
                {relinkDevice
                  ? "Enter the new 6-digit code shown on this board. Its name, settings, API token and history stay with the existing device."
                  : "An unlinked device shows a 6-digit verification code on its screen once it is connected to Wi-Fi."}
              </p>
            </div>
            <CloseButton onClick={close} />
          </div>

          <div className="space-y-2">
            <p className="text-sm font-medium">Device verification code</p>
            <div className="flex gap-2" dir="ltr">
              {digits.map((digit, i) => (
                <input
                  key={i}
                  ref={(el) => {
                    inputs.current[i] = el;
                  }}
                  value={digit}
                  autoFocus={i === 0}
                  inputMode="numeric"
                  autoComplete={i === 0 ? "one-time-code" : "off"}
                  maxLength={CODE_LENGTH}
                  aria-label={`Digit ${i + 1}`}
                  disabled={busy}
                  onChange={(e) => {
                    const value = e.target.value.replace(/\D/g, "");
                    if (!value) setDigits((prev) => prev.map((d, j) => (j === i ? "" : d)));
                    // Typing into a filled box yields two digits; keep the new one.
                    else fill(i, digit && value.length === 2 ? value.replace(digit, "") || digit : value);
                  }}
                  onKeyDown={(e) => onKeyDown(i, e)}
                  onPaste={(e: ClipboardEvent<HTMLInputElement>) => {
                    e.preventDefault();
                    fill(i, e.clipboardData.getData("text"));
                  }}
                  onFocus={(e) => e.target.select()}
                  className="h-12 w-full min-w-0 rounded-xl border border-zinc-200 bg-zinc-50 text-center font-mono text-lg font-semibold outline-none transition focus:border-blue-500 focus:bg-white focus:ring-2 focus:ring-blue-500/20 disabled:opacity-50 dark:border-zinc-700 dark:bg-zinc-800 dark:focus:bg-zinc-900"
                />
              ))}
            </div>
            {error && <p className="text-sm text-red-600 dark:text-red-400">{error}</p>}
          </div>

          <NoCodeHelp />

          <div className="flex justify-end gap-2">
            <Button onClick={close} className={secondaryButton + " h-10 px-4"}>
              Cancel
            </Button>
            <Button onClick={submit} disabled={code.length !== CODE_LENGTH || busy} className={accentButton + " h-10 min-w-20 px-4"}>
              {busy ? "Connecting…" : relinkDevice ? "Reconnect" : "Add"}
            </Button>
          </div>
        </DialogPanel>
      </div>
    </Dialog>
  );
}

/** Points a board at this console over USB when it was built without (or with another) console address. */
function NoCodeHelp() {
  const [status, setStatus] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const origin = typeof window === "undefined" ? "" : window.location.origin;
  const local = /^https?:\/\/(localhost|127\.|\[::1\])/.test(origin);

  const pointBoard = async () => {
    if (local) return;
    setBusy(true);
    setStatus(null);
    try {
      const api = await loadSerialApi();
      if (!api) throw new Error("This browser has no Web Serial or WebUSB support.");
      const port = (await findKnownPort(api)) ?? (await api.requestPort());
      const reply = await sendSerialCommand(port, `console ${origin}`);
      const ok = reply.some((line) => {
        try {
          const value = JSON.parse(line) as { console?: { server?: string } };
          return value.console?.server === origin;
        } catch { return false; }
      });
      setStatus(ok ? "Done. The board shows its code once it is online." : "No reply from the board. Is the ESP32 Console firmware running?");
    } catch (err) {
      if (err instanceof DOMException && err.name === "NotFoundError") return;
      setStatus(errorMessage(err));
    } finally {
      setBusy(false);
    }
  };

  return (
    <div className="space-y-2 rounded-xl bg-zinc-50 p-3 text-xs text-zinc-600 dark:bg-zinc-800/60 dark:text-zinc-400">
      <p className="font-semibold text-zinc-800 dark:text-zinc-200">No code on the board? Connect it over USB.</p>
      <ol className="list-decimal space-y-1 pl-4">
        <li>Flash the latest firmware from the Flash firmware tab.</li>
        <li>Connect the board to Wi-Fi (it opens a setup network on first start).</li>
        <li>The code appears on the home screen and under Menu → Console.</li>
      </ol>
      <p>
        The board must know this console’s address. Plug it in via USB and send it:
        <span className="mt-1 block break-all font-mono text-[11px]">{origin}</span>
      </p>
      {local && (
        <p className="text-amber-700 dark:text-amber-400">
          The board cannot reach localhost. Open this console through your computer’s network address (for example
          http://192.168.1.10:3000) or a deployed HTTPS address, then send that address over USB.
        </p>
      )}
      <Button onClick={pointBoard} disabled={busy || local} className={secondaryButton + " h-9 w-full bg-white dark:bg-zinc-900"}>
        {busy ? "Sending…" : "Send address over USB"}
      </Button>
      {status && <p>{status}</p>}
    </div>
  );
}
