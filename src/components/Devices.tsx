"use client";

import { Button, Menu, MenuButton, MenuItem, MenuItems } from "@headlessui/react";
import { signOut, useSession } from "next-auth/react";
import { useEffect, useState, type FormEvent } from "react";
import { AddDeviceDialog } from "@/components/AddDeviceDialog";
import { AuthCard } from "@/components/AuthCard";
import { ConfigureDialog } from "@/components/ConfigureDialog";
import { DeviceDetails, type DetailsTab } from "@/components/DeviceDetails";
import { ConfirmDialog, ErrorText, Sheet, Skeleton, ToastBanner, cardClass, inputClass, primaryButton, secondaryButton, useToast } from "@/components/ui";
import {
  LOW_BATTERY,
  api,
  availableUpdate,
  boardName,
  deviceName,
  isOtaActive,
  sendCommand,
  timeAgo,
} from "@/lib/device-client";
import type { PublicDevice } from "@/lib/device-types";
import { errorMessage } from "@/lib/esp";

type Dialog =
  | { kind: "configure"; id: string }
  | { kind: "details"; id: string; tab: DetailsTab }
  | { kind: "message"; id: string }
  | { kind: "confirm"; id: string; action: "restart" | "poweroff" | "remove" };

export function Devices({ active }: { active: boolean }) {
  const { data: session, status } = useSession();
  const user = session?.user;
  const [devices, setDevices] = useState<PublicDevice[] | null>(null);
  const [connection, setConnection] = useState<"connecting" | "live" | "retrying">("connecting");
  const [loadError, setLoadError] = useState<string | null>(null);
  const [addOpen, setAddOpen] = useState(false);
  const [dialog, setDialog] = useState<Dialog | null>(null);
  const [query, setQuery] = useState("");
  const [toast, setToast] = useToast();

  // Live list while the tab is visible: each request holds briefly and returns the current list,
  // so the browser sees changes almost immediately without a fixed polling interval.
  useEffect(() => {
    if (!active || !user) return;
    let source: EventSource | null = null;
    let retryTimer: ReturnType<typeof setTimeout> | null = null;
    let stopped = false;
    let failures = 0;
    let hasSnapshot = false;
    api<{ devices: PublicDevice[] }>("/api/devices")
      .then(({ devices }) => {
        if (stopped) return;
        hasSnapshot = true;
        setDevices(devices);
        setLoadError(null);
      })
      .catch((err) => { if (!stopped && !hasSnapshot) setLoadError(errorMessage(err)); });
    const open = () => {
      if (stopped) return;
      source?.close();
      source = new EventSource("/api/devices/stream");
      source.addEventListener("devices", (event) => {
        hasSnapshot = true;
        setDevices(JSON.parse((event as MessageEvent).data));
        setLoadError(null);
        failures = 0;
        setConnection("live");
      });
      // The endpoint intentionally closes each short request. Reopen quietly; only surface a
      // warning after repeated requests fail without yielding any device snapshot.
      source.onerror = () => {
        source?.close();
        failures += 1;
        if (failures >= 3) setConnection("retrying");
        retryTimer = setTimeout(() => { if (document.visibilityState === "visible") open(); }, Math.min(4000, 500 * failures));
      };
    };
    const onVisibility = () => {
      if (document.visibilityState === "visible") open();
      else source?.close();
    };
    open();
    document.addEventListener("visibilitychange", onVisibility);
    return () => {
      stopped = true;
      if (retryTimer) clearTimeout(retryTimer);
      source?.close();
      document.removeEventListener("visibilitychange", onVisibility);
    };
  }, [active, user]);

  if (status === "loading") return <p className="py-8 text-center text-sm text-zinc-500">Loading…</p>;
  if (!user) return <AuthCard />;

  const current = dialog ? devices?.find((d) => d.id === dialog.id) : undefined;

  const command = async (device: PublicDevice, body: Record<string, string>, done: string) => {
    try {
      await sendCommand(device, body);
      setToast(device.online ? done : `${done} when the device comes online`);
    } catch (err) {
      setToast(errorMessage(err));
    }
  };

  const confirm = async () => {
    if (dialog?.kind !== "confirm" || !current) return;
    setDialog(null);
    if (dialog.action === "remove") {
      await api(`/api/devices/${current.id}`, "DELETE").catch((err) => setToast(errorMessage(err)));
    } else {
      await command(current, { type: dialog.action }, dialog.action === "restart" ? "Restarting" : "Powering off");
    }
  };

  return (
    <div className="flex flex-1 flex-col gap-4">
      <section className="flex items-center justify-between gap-3">
        <div className="min-w-0">
          <h2 className="text-sm font-semibold">Devices</h2>
          <p className="truncate text-xs text-zinc-500">
            {devices === null
              ? loadError ? "Could not load devices" : "Loading…"
              : `${devices.length} linked · ${devices.filter((d) => d.online).length} online`}
            {connection === "retrying" && " · reconnecting…"}
          </p>
        </div>
        <div className="flex shrink-0 items-center gap-1">
          <Menu>
            <MenuButton className="h-10 max-w-28 truncate rounded-xl px-3 text-sm text-zinc-500 data-hover:bg-zinc-100 dark:data-hover:bg-zinc-800">
              {user.name ?? user.email}
            </MenuButton>
            <MenuItems anchor="bottom end" className={menuItemsClass}>
              <MenuItem>
                <button onClick={() => signOut()} className={menuItemClass}>
                  Sign out
                </button>
              </MenuItem>
            </MenuItems>
          </Menu>
          <Button onClick={() => setAddOpen(true)} className={primaryButton + " h-10 px-4"}>
            + Add Device
          </Button>
        </div>
      </section>

      {loadError && (
        <div role="alert" className="flex flex-wrap items-center justify-between gap-2 rounded-xl bg-red-50 p-3 text-sm text-red-700 dark:bg-red-950/50 dark:text-red-300">
          <span>{loadError}</span>
          <Button onClick={() => {
            setLoadError(null);
            void api<{ devices: PublicDevice[] }>("/api/devices")
              .then(({ devices }) => { setDevices(devices); setConnection("connecting"); })
              .catch((err) => setLoadError(errorMessage(err)));
          }} className="font-medium underline">Retry</Button>
        </div>
      )}

      {devices === null && !loadError && (
        <div className="space-y-3" aria-hidden>
          <Skeleton className="h-[168px] w-full" />
          <Skeleton className="h-[168px] w-full" />
        </div>
      )}

      {devices?.length === 0 && (
        <section className="flex flex-col items-center gap-3 rounded-2xl border border-dashed border-zinc-300 p-8 text-center dark:border-zinc-700">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" className="size-8 text-zinc-400" aria-hidden>
            <rect x="6" y="2.5" width="12" height="19" rx="2.5" />
            <path d="M10.5 18.5h3" strokeLinecap="round" />
          </svg>
          <div className="space-y-1">
            <p className="text-sm font-medium">No devices yet</p>
            <p className="text-xs text-zinc-500">
              Flash the firmware, connect the board to Wi-Fi and enter the 6-digit code from its screen.
            </p>
          </div>
        </section>
      )}

      {devices && devices.length > 1 && (
        <input
          value={query}
          onChange={(e) => setQuery(e.target.value)}
          placeholder="Search devices"
          aria-label="Search devices"
          className={inputClass}
        />
      )}

      {(() => {
        const filtered = devices?.filter((d) => {
          const q = query.trim().toLowerCase();
          return !q || deviceName(d).toLowerCase().includes(q) || boardName(d.board).toLowerCase().includes(q);
        });
        if (devices && devices.length > 0 && filtered?.length === 0) {
          return <p className="py-6 text-center text-sm text-zinc-500">No devices match “{query}”.</p>;
        }
        return filtered?.map((device) => (
          <DeviceCard
            key={device.id}
            device={device}
            onDialog={setDialog}
            onIdentify={() => command(device, { type: "identify" }, "Identifying")}
          />
        ));
      })()}

      <ToastBanner toast={toast} />

      <AddDeviceDialog
        open={addOpen}
        onClose={() => setAddOpen(false)}
        onAdded={(device) => {
          setAddOpen(false);
          setToast(`${deviceName(device)} added`);
        }}
      />

      {dialog?.kind === "configure" && current && (
        // Remount once the board reported its settings, so the form starts from real values.
        <ConfigureDialog key={String(current.settingsReported)} device={current} onClose={() => setDialog(null)} />
      )}
      {dialog?.kind === "details" && current && (
        <DeviceDetails device={current} initialTab={dialog.tab} onClose={() => setDialog(null)} />
      )}
      {dialog?.kind === "message" && current && (
        <MessageDialog
          device={current}
          onClose={() => setDialog(null)}
          onSent={() => {
            setDialog(null);
            setToast(current.online ? "Message sent" : "Message will be shown when the device comes online");
          }}
        />
      )}
      {dialog?.kind === "confirm" && current && (
        <ConfirmDialog
          open
          onClose={() => setDialog(null)}
          onConfirm={confirm}
          title={{ restart: "Restart device?", poweroff: "Power off device?", remove: "Remove device?" }[dialog.action]}
          confirmLabel={{ restart: "Restart", poweroff: "Power off", remove: "Remove" }[dialog.action]}
          description={
            <>
              {dialog.action === "restart" && `${deviceName(current)} restarts and reconnects within about 10 seconds.`}
              {dialog.action === "poweroff" &&
                `${deviceName(current)} switches off. It can only be switched on again with its power button.`}
              {dialog.action === "remove" &&
                `${deviceName(current)} is unlinked from your account. If it is online it shows a new verification code right away, so you can add it again.`}
            </>
          }
        />
      )}
    </div>
  );
}

function DeviceCard({
  device,
  onDialog,
  onIdentify,
}: {
  device: PublicDevice;
  onDialog: (dialog: Dialog) => void;
  onIdentify: () => void;
}) {
  const name = deviceName(device);
  const update = availableUpdate(device);
  const lowBattery = device.battery >= 0 && device.battery < LOW_BATTERY && !device.charging;
  const ota = device.ota;
  const installing = isOtaActive(ota);

  return (
    <section className={cardClass}>
      <div className="flex items-center gap-3 p-4">
        <div className="flex size-10 shrink-0 items-center justify-center rounded-xl bg-violet-100 text-sm font-semibold text-violet-700 dark:bg-violet-950 dark:text-violet-300">
          {name.charAt(0).toUpperCase()}
        </div>
        <div className="min-w-0 flex-1">
          <h3 className="truncate text-sm font-semibold">{name}</h3>
          <p className="flex items-center gap-1.5 truncate text-xs text-zinc-500">
            <span
              className={`size-2 shrink-0 rounded-full ${device.online ? "bg-emerald-500" : "bg-zinc-300 dark:bg-zinc-600"}`}
            />
            {device.online ? "Online" : `Offline · ${timeAgo(device.lastSeen)}`}
            {device.syncing && <span className="text-amber-600 dark:text-amber-400">· Applying changes</span>}
          </p>
        </div>
        <Menu>
          <MenuButton
            className="rounded-lg p-2 text-zinc-500 data-hover:bg-zinc-100 dark:data-hover:bg-zinc-800"
            aria-label="More actions"
          >
            <svg viewBox="0 0 20 20" fill="currentColor" className="size-5" aria-hidden>
              <path d="M4 10a1.5 1.5 0 1 1-3 0 1.5 1.5 0 0 1 3 0Zm7.5 0a1.5 1.5 0 1 1-3 0 1.5 1.5 0 0 1 3 0ZM19 10a1.5 1.5 0 1 1-3 0 1.5 1.5 0 0 1 3 0Z" />
            </svg>
          </MenuButton>
          <MenuItems anchor="bottom end" className={menuItemsClass}>
            <MenuItem>
              <button onClick={onIdentify} className={menuItemClass}>
                Identify (beep & flash)
              </button>
            </MenuItem>
            <MenuItem>
              <button onClick={() => onDialog({ kind: "message", id: device.id })} className={menuItemClass}>
                Send message
              </button>
            </MenuItem>
            <MenuItem>
              <button onClick={() => onDialog({ kind: "details", id: device.id, tab: "check" })} className={menuItemClass}>
                Hardware check
              </button>
            </MenuItem>
            <MenuItem>
              <button onClick={() => onDialog({ kind: "details", id: device.id, tab: "wifi" })} className={menuItemClass}>
                Wi-Fi networks
              </button>
            </MenuItem>
            <MenuItem>
              <button
                onClick={() => onDialog({ kind: "confirm", id: device.id, action: "restart" })}
                className={menuItemClass}
              >
                Restart
              </button>
            </MenuItem>
            <MenuItem>
              <button
                onClick={() => onDialog({ kind: "confirm", id: device.id, action: "poweroff" })}
                className={menuItemClass}
              >
                Power off
              </button>
            </MenuItem>
            <MenuItem>
              <button
                onClick={() => onDialog({ kind: "confirm", id: device.id, action: "remove" })}
                className={menuItemClass + " text-red-600 data-focus:bg-red-50 dark:text-red-400 dark:data-focus:bg-red-950/40"}
              >
                Remove
              </button>
            </MenuItem>
          </MenuItems>
        </Menu>
      </div>

      <dl className="mx-4 grid grid-cols-3 divide-x divide-zinc-200 rounded-xl bg-zinc-50 py-2.5 text-xs dark:divide-zinc-800 dark:bg-zinc-800/50">
        <Stat label="Model" value={boardName(device.board)} />
        <Stat label="Firmware" value={device.firmware ? `v${device.firmware}` : "—"} />
        <Stat
          label="Battery"
          value={device.battery >= 0 ? `${device.battery}%${device.charging ? " ⚡" : ""}` : "—"}
          warn={lowBattery}
        />
      </dl>

      {(update || installing || lowBattery) && (
        <div className="mx-4 mt-2 space-y-1.5">
          {lowBattery && (
            <p className="rounded-lg bg-red-50 px-3 py-2 text-xs text-red-700 dark:bg-red-950/40 dark:text-red-300">
              Battery low. Charge the device soon.
            </p>
          )}
          {(update || installing) && (
            <button
              onClick={() => onDialog({ kind: "details", id: device.id, tab: "firmware" })}
              className="flex w-full items-center justify-between rounded-lg bg-blue-50 px-3 py-2 text-left text-xs text-blue-700 dark:bg-blue-950/40 dark:text-blue-300"
            >
              <span>
                {installing && ota ? `Installing v${ota.version}… ${ota.progress}%` : `Update available: v${update!.version}`}
              </span>
              <span aria-hidden>›</span>
            </button>
          )}
        </div>
      )}

      <div className="mt-3 grid grid-cols-2 gap-2 border-t border-zinc-200 p-3 dark:border-zinc-800">
        <Button onClick={() => onDialog({ kind: "configure", id: device.id })} className={secondaryButton + " h-10"}>
          Configure
        </Button>
        <Button
          onClick={() => onDialog({ kind: "details", id: device.id, tab: "status" })}
          className={secondaryButton + " h-10"}
        >
          Details
        </Button>
      </div>
    </section>
  );
}

function Stat({ label, value, warn }: { label: string; value: string; warn?: boolean }) {
  return (
    <div className="min-w-0 px-3">
      <dt className="text-zinc-500">{label}</dt>
      <dd className={`truncate font-medium ${warn ? "text-red-600 dark:text-red-400" : ""}`}>{value}</dd>
    </div>
  );
}

function MessageDialog({ device, onClose, onSent }: { device: PublicDevice; onClose: () => void; onSent: () => void }) {
  const [text, setText] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  const submit = async (event: FormEvent) => {
    event.preventDefault();
    setBusy(true);
    setError(null);
    try {
      await sendCommand(device, { type: "notify", text });
      onSent();
    } catch (err) {
      setError(errorMessage(err));
    } finally {
      setBusy(false);
    }
  };

  return (
    <Sheet open onClose={onClose} title="Send message" subtitle={`Shown on the screen of ${deviceName(device)} for at least 5 seconds. Long messages scroll on firmware v1.0.3 or later.`}>
      <form onSubmit={submit} className="space-y-3">
        <input
          value={text}
          onChange={(e) => setText(e.target.value)}
          maxLength={60}
          placeholder="Dinner is ready"
          autoFocus
          className={inputClass}
        />
        <p className="text-xs text-zinc-500">
          English letters, digits and punctuation only; the device font has no other characters.
        </p>
        <ErrorText>{error}</ErrorText>
        <Button type="submit" disabled={busy || !text.trim()} className={primaryButton + " h-11 w-full"}>
          {busy ? "Sending…" : "Send"}
        </Button>
      </form>
    </Sheet>
  );
}

const menuItemsClass =
  "z-20 w-52 rounded-xl border border-zinc-200 bg-white p-1 text-sm shadow-lg outline-none [--anchor-gap:4px] dark:border-zinc-800 dark:bg-zinc-900";
const menuItemClass = "w-full rounded-lg px-3 py-2 text-left data-focus:bg-zinc-100 dark:data-focus:bg-zinc-800";
