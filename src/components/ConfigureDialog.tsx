"use client";

import { Button } from "@headlessui/react";
import { useState } from "react";
import { LedRingDesigner } from "@/components/LedRingDesigner";
import { SdFileManager, formatBytes, runFileJob } from "@/components/SdFileManager";
import { Select } from "@/components/Select";
import { ConfirmDialog, ErrorText, Group, Sheet, Slider, Toggle, ToastBanner, accentButton, inputClass, secondaryButton, useToast } from "@/components/ui";
import { api, boardName, compareVersions, deviceName, duration } from "@/lib/device-client";
import { LED_COLORS, POWER_OFF_OPTIONS, SLEEP_OPTIONS, TIME_ZONES, type DeviceSettings } from "@/lib/device-settings";
import { SD_FILES_FIRMWARE, type PublicDevice } from "@/lib/device-types";
import { errorMessage } from "@/lib/esp";

export function ConfigureDialog({
  device,
  onClose,
}: {
  device: PublicDevice;
  onClose: () => void;
}) {
  const [name, setName] = useState(device.name);
  const [settings, setSettings] = useState<DeviceSettings>(device.settings);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [files, setFiles] = useState(false);
  const [confirmFormat, setConfirmFormat] = useState(false);
  const [formatting, setFormatting] = useState(false);
  const [toast, setToast] = useToast();
  const modern = compareVersions(device.firmware, SD_FILES_FIRMWARE) >= 0;
  const format = async () => {
    setConfirmFormat(false);
    setFormatting(true);
    setError(null);
    try {
      await runFileJob(device, { type: "sd_format" });
      setToast("SD card formatted");
    } catch (err) {
      setError(errorMessage(err));
    } finally {
      setFormatting(false);
    }
  };
  const set = <K extends keyof DeviceSettings>(key: K, value: DeviceSettings[K]) =>
    setSettings((prev) => ({ ...prev, [key]: value }));

  const changed = JSON.stringify(settings) !== JSON.stringify(device.settings);

  const save = async () => {
    setBusy(true);
    setError(null);
    try {
      const editable = { ...settings };
      Reflect.deleteProperty(editable, "project");
      await api(`/api/devices/${device.id}`, "PATCH", { name, ...(changed && { settings: editable }) });
      onClose();
    } catch (err) {
      setError(errorMessage(err));
    } finally {
      setBusy(false);
    }
  };

  return (
    <Sheet
      open
      onClose={onClose}
      title={`Configure ${deviceName(device)}`}
      subtitle={device.online ? "Changes reach the device within a second." : "Device is offline. Changes apply when it reconnects."}
      footer={
        <>
          <ErrorText>{error}</ErrorText>
          <Button onClick={onClose} className={secondaryButton + " h-10 px-4"}>
            Cancel
          </Button>
          <Button
            onClick={save}
            disabled={busy || !device.settingsReported || (!changed && name === device.name)}
            className={accentButton + " h-10 min-w-20 px-4"}
          >
            {busy ? "Saving…" : "Save"}
          </Button>
        </>
      }
    >
      {!device.settingsReported ? (
        <p className="rounded-xl bg-zinc-50 p-3 text-sm text-zinc-500 dark:bg-zinc-800/60">
          Waiting for the device to report its current settings…
        </p>
      ) : (
        <>
          <label className="block space-y-1.5 text-xs font-medium text-zinc-500">
            <span>Name</span>
            <input
              value={name}
              maxLength={40}
              placeholder={boardName(device.board)}
              onChange={(e) => setName(e.target.value)}
              className={inputClass}
            />
          </label>

          <Group title="Display">
            <Select
              label="Theme"
              value={settings.theme}
              options={["dark", "light"] as DeviceSettings["theme"][]}
              onChange={(v) => set("theme", v)}
              getKey={(v) => v}
              renderValue={(v) => (v === "dark" ? "Dark" : "Light")}
            />
            <Slider label="Brightness" value={settings.brightness} min={10} max={100} step={10} unit="%" onChange={(v) => set("brightness", v)} />
            <Select
              label="Auto sleep"
              value={settings.sleep_s}
              options={SLEEP_OPTIONS}
              onChange={(v) => set("sleep_s", v)}
              getKey={String}
              renderValue={duration}
            />
            <Select
              label="Auto off on battery"
              value={settings.power_off_s}
              options={POWER_OFF_OPTIONS}
              onChange={(v) => set("power_off_s", v)}
              getKey={String}
              renderValue={duration}
            />
            <Toggle label="Rotate 180°" checked={settings.rotate} onChange={(v) => set("rotate", v)} />
            <Toggle label="Show battery %" checked={settings.bat_pct} onChange={(v) => set("bat_pct", v)} />
          </Group>

          <Group title="Sound & LED">
            <Slider label="Volume" value={settings.volume} min={0} max={100} step={10} onChange={(v) => set("volume", v)} />
            <Toggle label="Button sound" checked={settings.button_sound} onChange={(v) => set("button_sound", v)} />
            <Toggle label="LED ring" checked={settings.led_on} onChange={(v) => set("led_on", v)} />
            <Toggle
              label="LED feedback"
              description="Glow on press, low battery pulse, charging"
              checked={settings.led_feedback}
              onChange={(v) => set("led_feedback", v)}
            />
            <Slider label="LED brightness" value={settings.led_brightness} min={1} max={8} step={1} unit="/8" onChange={(v) => set("led_brightness", v)} />
            {!modern && (
              <Select
                label="LED color"
                value={settings.led_color}
                options={LED_COLORS.map((_, i) => i)}
                onChange={(v) => set("led_color", v)}
                getKey={String}
                renderValue={(v) => LED_COLORS[v]}
              />
            )}
          </Group>

          {modern ? (
            <Group title="LED ring designer">
              <LedRingDesigner value={settings} onChange={(next) => setSettings((prev) => ({ ...prev, ...next }))} />
            </Group>
          ) : (
            <p className="rounded-xl bg-zinc-50 p-3 text-xs text-zinc-500 dark:bg-zinc-800/60">Update the base firmware to v{SD_FILES_FIRMWARE} for per-LED colors, blinking and ring effects.</p>
          )}

          <Group title="SD card">
            <p className="text-sm text-zinc-500">
              {!device.sdCard ? "Not reported by this firmware." : device.sdCard.mounted ? `${formatBytes(device.sdCard.free)} free of ${formatBytes(device.sdCard.total)}` : "No card detected. Operations check the card again."}
            </p>
            {modern ? (
              <div className="flex flex-wrap gap-2">
                <Button onClick={() => setFiles(true)} disabled={formatting} className={secondaryButton + " h-10 px-4"}>Open file manager</Button>
                <Button onClick={() => setConfirmFormat(true)} disabled={formatting} className="h-10 rounded-xl border border-red-200 px-4 text-sm font-medium text-red-600 data-disabled:opacity-50 data-hover:bg-red-50 dark:border-red-900 dark:data-hover:bg-red-950">
                  {formatting ? "Formatting… waiting for the board" : "Format card"}
                </Button>
              </div>
            ) : (
              <p className="text-xs text-zinc-500">Update the base firmware to v{SD_FILES_FIRMWARE} to browse and format the card.</p>
            )}
          </Group>

          <Group title="Connectivity & time">
            <Toggle label="Bluetooth" checked={settings.ble_on} onChange={(v) => set("ble_on", v)} />
            <Toggle label="Clock" checked={settings.clock_on} onChange={(v) => set("clock_on", v)} />
            <Select
              label="Time zone"
              value={TIME_ZONES.find((z) => z.id === settings.tz) ?? TIME_ZONES[0]}
              options={TIME_ZONES}
              onChange={(z) => set("tz", z.id)}
              getKey={(z) => z.id}
              renderValue={(z) => z.label}
            />
          </Group>
        </>
      )}
      {files && <SdFileManager device={device} onClose={() => setFiles(false)} />}
      <ConfirmDialog
        open={confirmFormat}
        onClose={() => setConfirmFormat(false)}
        onConfirm={format}
        title="Format the SD card?"
        description="Every file on the card is erased, including images and videos of installed display projects; those projects show “Insert SD card” until reinstalled. This cannot be undone."
        confirmLabel="Erase and format"
      />
      <ToastBanner toast={toast} />
    </Sheet>
  );
}
