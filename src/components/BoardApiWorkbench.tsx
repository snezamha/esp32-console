"use client";

import { Button } from "@headlessui/react";
import { useSession } from "next-auth/react";
import { useEffect, useRef, useState } from "react";
import { AuthCard } from "@/components/AuthCard";
import { LedRingDesigner } from "@/components/LedRingDesigner";
import { Select } from "@/components/Select";
import { ErrorText, Slider, accentButton, cardClass, inputClass, secondaryButton } from "@/components/ui";
import { api, deviceName } from "@/lib/device-client";
import { DEFAULT_MUSIC_URL } from "@/lib/board-control";
import { DEFAULT_SETTINGS, LED_COLORS, LED_MODES, type DeviceSettings } from "@/lib/device-settings";
import type { PublicDevice } from "@/lib/device-types";
import { errorMessage } from "@/lib/esp";

const MUSIC_REQUEST = { settings: { volume: 70 }, audio: { type: "music", url: DEFAULT_MUSIC_URL } };

const EXAMPLES = [
  { name: "Display and volume", body: { settings: { theme: "light", brightness: 70, volume: 40 } } },
  { name: "RGB ring", body: { settings: { led_on: true, led_rgb: "#ff6600", led_mode: "breathe", led_speed: 5 } } },
  { name: "Individual LEDs", body: { settings: { led_on: true, led_pixels_rgb: Array.from({ length: 8 }, (_, i) => ({ r: i % 2 ? 0 : 255, g: 0, b: i % 2 ? 255 : 0, level: 80, blink: i % 2 === 0 })) } } },
  { name: "Beep", body: { audio: { type: "beep", frequency: 880, durationMs: 300 } } },
  { name: "MP3 URL", body: MUSIC_REQUEST },
] as const;

type Result = { status: number; elapsed: number; body: string };

export function BoardApiWorkbench({ active }: { active: boolean }) {
  const { data: session, status } = useSession();
  const [devices, setDevices] = useState<PublicDevice[]>([]);
  const [selectedId, setSelectedId] = useState("");
  const selectedIdRef = useRef("");
  const [enabled, setEnabled] = useState(false);
  const [token, setToken] = useState("");
  const [showToken, setShowToken] = useState(false);
  const [method, setMethod] = useState<"GET" | "POST">("POST");
  const [body, setBody] = useState(JSON.stringify(MUSIC_REQUEST, null, 2));
  const [file, setFile] = useState<File | null>(null);
  const [result, setResult] = useState<Result | null>(null);
  const [pendingRequest, setPendingRequest] = useState<{ id: string; status: string } | null>(null);
  const [preview, setPreview] = useState<DeviceSettings>(DEFAULT_SETTINGS);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");

  useEffect(() => {
    if (!active || !session?.user) return;
    let live = true;
    api<{ devices: PublicDevice[] }>("/api/devices")
      .then(({ devices }) => {
        if (!live) return;
        setDevices(devices);
        const selected = devices.find((device) => device.id === selectedIdRef.current)
          ?? devices.find((device) => device.activeProject === "board-control-api") ?? devices[0];
        if (selectedIdRef.current !== selected?.id) { setToken(""); setEnabled(false); setResult(null); setPendingRequest(null); }
        selectedIdRef.current = selected?.id ?? "";
        setSelectedId(selected?.id ?? "");
        if (selected) setPreview(selected.settings);
      })
      .catch((cause) => { if (live) setError(errorMessage(cause)); });
    return () => { live = false; };
  }, [active, session?.user]);

  const device = devices.find((entry) => entry.id === selectedId);
  const installed = device?.activeProject === "board-control-api";
  const endpoint = typeof window === "undefined" || !device ? "" : `${window.location.origin}/api/board-control/${device.id}`;

  useEffect(() => {
    if (!selectedId || !installed) return;
    let live = true;
    api<{ enabled: boolean }>(`/api/devices/${selectedId}/control-key`)
      .then(({ enabled }) => { if (live) setEnabled(enabled); })
      .catch((cause) => { if (live) setError(errorMessage(cause)); });
    return () => { live = false; };
  }, [selectedId, installed]);

  const rotate = async () => {
    if (!device) return;
    setBusy(true); setError("");
    try {
      const response = await api<{ key: string }>(`/api/devices/${device.id}/control-key`, "POST");
      setToken(response.key); setEnabled(true); setShowToken(true); setPendingRequest(null);
    } catch (cause) { setError(errorMessage(cause)); } finally { setBusy(false); }
  };
  const revoke = async () => {
    if (!device) return;
    setBusy(true); setError("");
    try {
      await api(`/api/devices/${device.id}/control-key`, "DELETE");
      setToken(""); setEnabled(false); setResult(null); setPendingRequest(null);
    } catch (cause) { setError(errorMessage(cause)); } finally { setBusy(false); }
  };
  const send = async () => {
    if (!device || !token || busy || pendingRequest) return;
    setBusy(true); setError(""); setResult(null); setPendingRequest(null);
    try {
      let payload: unknown;
      if (method === "POST") {
        if (file) {
          if (file.size > 2 * 1024 * 1024) throw new Error("MP3 file must be at most 2 MB.");
          const data = await new Promise<string>((resolve, reject) => {
            const reader = new FileReader();
            reader.onload = () => resolve(String(reader.result).split(",")[1] ?? "");
            reader.onerror = () => reject(new Error("Could not read the MP3 file."));
            reader.readAsDataURL(file);
          });
          payload = { audio: { type: "music", data } };
        } else {
          try { payload = JSON.parse(body); } catch { throw new Error("Request body is not valid JSON."); }
        }
      }
      const started = performance.now();
      const response = await fetch(endpoint, { method, headers: {
        Authorization: `Bearer ${token}`,
        ...(method === "POST" ? { "Content-Type": "application/json" } : {}),
      }, ...(method === "POST" ? { body: JSON.stringify(payload) } : {}) });
      let finalResponse = response;
      let text = await response.text();
      if (method === "POST" && response.status === 202) {
        const queued = JSON.parse(text) as { requestId?: string; status?: string };
        if (queued.requestId) {
          setPendingRequest({ id: queued.requestId, status: queued.status ?? "queued" });
          while (true) {
            await new Promise((resolve) => setTimeout(resolve, 1000));
            const check = await fetch(`${endpoint}?requestId=${encodeURIComponent(queued.requestId)}`, { headers: { Authorization: `Bearer ${token}` } });
            finalResponse = check;
            text = await check.text();
            if (check.status === 202) {
              const progress = JSON.parse(text) as { status?: string };
              setPendingRequest({ id: queued.requestId, status: progress.status ?? "sent" });
            }
            if (check.status !== 202) break;
          }
        }
      }
      setPendingRequest(null);
      let formatted = text;
      try { formatted = JSON.stringify(JSON.parse(text), null, 2); } catch { /* Show raw response. */ }
      setResult({ status: finalResponse.status, elapsed: Math.round(performance.now() - started), body: formatted });
      if (response.ok && method === "GET") {
        const parsed = JSON.parse(text) as { settings?: Partial<DeviceSettings> };
        if (parsed.settings) setPreview((current) => ({ ...current, ...parsed.settings }));
      }
    } catch (cause) { setError(errorMessage(cause)); } finally { setBusy(false); }
  };
  const checkPending = async () => {
    if (!pendingRequest || !token || !device) return;
    setBusy(true); setError("");
    try {
      const response = await fetch(`${endpoint}?requestId=${encodeURIComponent(pendingRequest.id)}`, { headers: { Authorization: `Bearer ${token}` } });
      const text = await response.text();
      if (response.status === 202) {
        const progress = JSON.parse(text) as { status?: string };
        setPendingRequest({ ...pendingRequest, status: progress.status ?? "sent" });
      } else {
        setPendingRequest(null);
        let formatted = text;
        try { formatted = JSON.stringify(JSON.parse(text), null, 2); } catch { /* Show raw response. */ }
        setResult({ status: response.status, elapsed: 0, body: formatted });
      }
    } catch (cause) { setError(errorMessage(cause)); } finally { setBusy(false); }
  };
  const example = (value: unknown) => { setMethod("POST"); setFile(null); setBody(JSON.stringify(value, null, 2)); setResult(null); };
  const rgb = preview.led_pixels ? `#${preview.led_pixels.slice(0, 6)}` : `#${["ffffff", "ff0000", "00ff00", "0000ff", "00ffff", "a000ff", "ff5a00"][preview.led_color] ?? "ffffff"}`;

  if (status === "loading") return <p className="py-8 text-center text-sm text-zinc-500">Loading…</p>;
  if (!session?.user) return <AuthCard />;
  return <div className="space-y-4">
    <div><h2 className="text-sm font-semibold">Board API workbench</h2><p className="mt-1 text-xs text-zinc-500">Build a JSON request, send it to your board and inspect the response. The board does not display this interface.</p></div>
    <ErrorText>{error}</ErrorText>
    {!devices.length ? <p className={cardClass + " p-4 text-sm text-zinc-500"}>Add a board in Devices, then install Board control API from Projects.</p> : <>
      <section className={cardClass + " space-y-3 p-4"}>
        <Select label="Board" value={device ?? devices[0]} options={devices} onChange={(next) => { selectedIdRef.current = next.id; setSelectedId(next.id); setPreview(next.settings); setToken(""); setEnabled(false); setResult(null); setPendingRequest(null); }} getKey={(entry) => entry.id} renderValue={(entry) => `${deviceName(entry)} · ${entry.online ? "Online" : "Offline"}`} />
        {!installed && <p className="text-xs text-amber-600">Install Board control API on this board in Projects before generating a token.</p>}
        {installed && device.activeProjectVersion.localeCompare("1.0.5", undefined, { numeric: true }) < 0 && <p className="text-xs text-amber-600">Update Board control API to v1.0.5 and base firmware to v1.1.27 to receive board-confirmed responses.</p>}
        {installed && !device.online && <p className="text-xs text-amber-600">The board is offline. Queued requests expire after 90 seconds unless it reconnects.</p>}
        {installed && <>
          <div className="flex flex-wrap gap-2">
            <Button disabled={busy} onClick={() => void rotate()} className={secondaryButton + " px-3 py-2"}>{enabled ? "Rotate token" : "Generate token"}</Button>
            {enabled && <Button disabled={busy} onClick={() => void revoke()} className={secondaryButton + " px-3 py-2"}>Revoke token</Button>}
          </div>
          {enabled && !token && <p className="text-xs text-zinc-500">A token is active. Paste your saved token or rotate it to see a new one.</p>}
          <label className="block space-y-1 text-xs"><span>API endpoint</span><input readOnly value={endpoint} className={inputClass + " font-mono text-xs"} /></label>
          <div className="flex gap-2"><label className="block flex-1 space-y-1 text-xs"><span>Bearer token</span><input type={showToken ? "text" : "password"} value={token} onChange={(event) => setToken(event.target.value.trim())} autoComplete="off" placeholder="Generate or paste your token" className={inputClass + " font-mono text-xs"} /></label><Button onClick={() => setShowToken((value) => !value)} className={secondaryButton + " mt-5 px-3"}>{showToken ? "Hide" : "Show"}</Button></div>
          <p className="text-xs text-zinc-500">Tokens are sent only in the Authorization header and are not saved in this browser. Copy the generated token before leaving this tab.</p>
        </>}
      </section>

      <section className={cardClass + " space-y-3 p-4"}>
        <h3 className="text-sm font-semibold">Request</h3>
        <div className="flex flex-wrap gap-2">{EXAMPLES.map((entry) => <Button key={entry.name} onClick={() => example(entry.body)} className={secondaryButton + " px-3 py-2 text-xs"}>{entry.name}</Button>)}</div>
        <div className="flex flex-wrap items-center gap-2"><span className="text-xs font-medium">Music controls:</span>{(["pause", "play", "stop"] as const).map((type) => <Button key={type} onClick={() => example({ audio: { type } })} className={secondaryButton + " px-3 py-2 text-xs"}>{type[0].toUpperCase() + type.slice(1)}</Button>)}</div>
        <div className="flex gap-2"><select aria-label="HTTP method" value={method} onChange={(event) => setMethod(event.target.value as "GET" | "POST")} className={inputClass + " w-28"}><option>GET</option><option>POST</option></select><input readOnly value={endpoint} aria-label="Request URL" className={inputClass + " min-w-0 flex-1 font-mono text-xs"} /></div>
        {method === "POST" && <>
          <label className="block space-y-1 text-xs"><span>JSON body</span><textarea value={body} onChange={(event) => { setBody(event.target.value); setFile(null); }} spellCheck={false} rows={9} className={inputClass + " h-auto min-h-44 py-3 font-mono text-xs"} /></label>
          <label className="block text-xs text-zinc-500">Or choose an MP3 file (up to 2 MB): <input type="file" accept="audio/mpeg,.mp3" onChange={(event) => { setFile(event.target.files?.[0] ?? null); }} className="mt-1 block w-full text-xs" /></label>
          {file && <p className="text-xs text-zinc-500">Sending {file.name} as base64 MP3 data. The JSON editor is ignored for this request.</p>}
        </>}
        <Button disabled={busy || Boolean(pendingRequest) || !installed || !token} onClick={() => void send()} className={accentButton + " h-10 px-5"}>{busy ? "Waiting for board…" : "Send request"}</Button>
        {pendingRequest && <p role="status" className="text-xs text-amber-600">{pendingRequest.status === "sent" ? "Request sent to the board; waiting for playback or action confirmation." : "Request is in the board queue."} Request ID: <code>{pendingRequest.id}</code></p>}
        {pendingRequest && !busy && <Button onClick={() => void checkPending()} className={secondaryButton + " px-3 py-2 text-xs"}>Check request status</Button>}
        {result && <div className="space-y-2"><p className={`text-xs font-medium ${result.status === 202 ? "text-amber-600" : result.status < 400 ? "text-emerald-600" : "text-red-600"}`}>HTTP {result.status} · {result.elapsed} ms</p><pre className="max-h-80 overflow-auto rounded-xl bg-zinc-100 p-3 font-mono text-xs dark:bg-zinc-800">{result.body}</pre></div>}
        {result?.status === 202 && <p className="text-xs text-zinc-500">Waiting for the board to confirm this action. New requests stay disabled until it finishes.</p>}
      </section>

      <section className={cardClass + " space-y-3 p-4"}>
        <h3 className="text-sm font-semibold">LED simulator</h3>
        <p className="text-xs text-zinc-500">Preview the ring and add its settings to the JSON request. Preview changes stay local until you send the request.</p>
        <div className="grid gap-3 sm:grid-cols-2"><Slider label="Display brightness" value={preview.brightness} min={10} max={100} step={10} unit="%" onChange={(brightness) => setPreview((current) => ({ ...current, brightness }))} /><Slider label="Volume" value={preview.volume} min={0} max={100} step={10} unit="%" onChange={(volume) => setPreview((current) => ({ ...current, volume }))} /></div>
        <label className="flex items-center gap-2 text-xs">Uniform RGB <input type="color" value={rgb} onChange={(event) => setPreview((current) => ({ ...current, led_on: true, led_pixels: Array(8).fill(`${event.target.value.slice(1).toLowerCase()}:100:0`).join(",") }))} className="size-9" /><span className="font-mono">{rgb.toUpperCase()}</span></label>
        <LedRingDesigner value={preview} onChange={(patch) => setPreview((current) => ({ ...current, ...patch }))} />
        <Button onClick={() => example({ settings: { brightness: preview.brightness, volume: preview.volume, led_on: preview.led_on, led_brightness: preview.led_brightness, led_mode: preview.led_mode, led_speed: preview.led_speed, led_pixels: preview.led_pixels } })} className={secondaryButton + " px-3 py-2"}>Use preview as JSON request</Button>
      </section>

    </>}
    <Guide />
  </div>;
}

function Guide() {
  return <section className={cardClass + " space-y-4 p-4 text-xs text-zinc-500"}>
    <h3 className="text-sm font-semibold text-zinc-900 dark:text-zinc-100">API guide</h3>
    <p>Install Board control API v1.0.5 with firmware v1.1.27 or newer. Generate a token above. Send <code>Authorization: Bearer &lt;token&gt;</code> with every request. GET reads the board state. POST requests run in order, one at a time.</p>
    <p>The board shows a text-free loading spinner when it receives a control action and while a music stream connects or buffers. A GET is served from the console database and does not contact the board, so it does not trigger the spinner.</p>
    <div className="overflow-x-auto"><table className="w-full min-w-[36rem] text-left"><thead><tr><th className="pb-2">Setting</th><th className="pb-2">Values</th></tr></thead><tbody className="align-top">{[
      ["theme, brightness, volume", "dark/light; 10–100%; 0–100%"],
      ["sleep_s, power_off_s", "sleep: 0, 10, 30, 60, 300 seconds; power off: 0, 60, 300, 600 seconds"],
      ["rotate, bat_pct, button_sound", "true or false"],
      ["ble_on, clock_on, tz", "true/false; time zone from Configure (for example Europe/Berlin)"],
      ["led_on, led_feedback", "true or false"],
      ["led_brightness, led_speed", "1–8 and 1–10"],
      ["led_mode", LED_MODES.join(", ")],
      ["led_rgb", "#RRGGBB for all 8 LEDs, for example #ff6600"],
      ["led_pixels_rgb", "Array of 8 objects: {r,g,b,level,blink}; RGB 0–255, level 0–100. Index 0 is upper left, then clockwise around the display."],
      ["led_pixels, led_color", "Compact Configure format; legacy palette index 0–6 (" + LED_COLORS.join(", ") + ")"],
    ].map(([name, values]) => <tr key={name} className="border-t border-zinc-200 dark:border-zinc-800"><td className="py-2 pr-4 font-mono text-zinc-800 dark:text-zinc-200">{name}</td><td className="py-2">{values}</td></tr>)}</tbody></table></div>
    <p>One POST body can contain <code>settings</code>, <code>audio</code>, or both. They are separate JSON fields because settings persist while audio is a one-time action. Send one audio action per request. The default MP3 URL is <code>{DEFAULT_MUSIC_URL}</code>; the stream does not need a <code>.mp3</code> extension.</p>
    <div className="overflow-x-auto"><table className="w-full min-w-[36rem] text-left"><thead><tr><th className="pb-2">Audio action in the same JSON body</th><th className="pb-2">Result</th></tr></thead><tbody className="align-top">{[
      ['{"audio":{"type":"music","url":"https://navairan.com/;stream.nsv"}}', "Start a direct MP3 stream"],
      ['{"audio":{"type":"music","data":"BASE64_MP3"}}', "Play an uploaded MP3 up to 2 MB"],
      ['{"audio":{"type":"beep","frequency":880,"durationMs":300}}', "Beep at 100–4000 Hz for 20–3000 ms"],
      ['{"audio":{"type":"pause"}}', "Pause the last music stream"],
      ['{"audio":{"type":"play"}}', "Play the last stream from its beginning, or start the default stream"],
      ['{"audio":{"type":"stop"}}', "Stop playback and clear the saved music URL"],
    ].map(([json, meaning]) => <tr key={json} className="border-t border-zinc-200 dark:border-zinc-800"><td className="py-2 pr-4 font-mono text-zinc-800 dark:text-zinc-200">{json}</td><td className="py-2">{meaning}</td></tr>)}</tbody></table></div>
    <p>Pause requires a previous music request. Play restarts the last stream because exact position seeking is unavailable; if no source is saved, it starts the default stream. Stop clears the saved source, so the next Play starts the default stream. HTTP 200 with <code>success: true</code> confirms the board completed the action. For music, completion means playback reached Playing. HTTP 202 gives a <code>requestId</code>; check <code>GET endpoint?requestId=…</code> until the board confirms or the request fails. Requests expire after 90 seconds without confirmation. The workbench waits and blocks a second Send request during that time.</p>
    <pre className="overflow-x-auto rounded-xl bg-zinc-100 p-3 font-mono text-xs dark:bg-zinc-800">{'curl -X POST "YOUR_API_ENDPOINT" \\\n  -H "Authorization: Bearer YOUR_TOKEN" \\\n  -H "Content-Type: application/json" \\\n  -d \'{"settings":{"volume":70},"audio":{"type":"music","url":"https://navairan.com/;stream.nsv"}}\''}</pre>
    <p>The token is separate from the device pairing token. Rotating it invalidates the previous token. The API works only while Board control API is the active project.</p>
  </section>;
}
