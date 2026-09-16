import type { ReactNode } from "react";

const codeClass = "overflow-x-auto whitespace-pre rounded-xl bg-zinc-950 p-3 font-mono text-xs leading-5 text-zinc-100";

export function ProjectBuilderGuide() {
  return (
    <div className="space-y-6 pb-2 text-sm leading-6 text-zinc-700 dark:text-zinc-300">
      <p>A Builder sketch is compiled into a standalone Xtensa ELF. It runs inside the project area and never replaces the base firmware. Include only the stable board header:</p>
      <Code>{`#include "project_builder.h"

void setup(ProjectBoard *board) {
  // Runs once after the project is loaded.
}

void loop(ProjectBoard *board) {
  // Runs once per display frame and must return quickly.
}

PROJECT_SKETCH()`}</Code>

      <Section title="Display, colors and time">
        <p>Coordinates are relative to the project area. Colors use RGB565. Current time is available through <Inline>hour</Inline>, <Inline>minute</Inline>, <Inline>second</Inline>, <Inline>year</Inline>, <Inline>month</Inline>, <Inline>day</Inline> and <Inline>weekday</Inline>.</p>
        <Code>{`uint16_t blue = project_rgb(20, 80, 255);
project_clear(board, project_rgb(0, 0, 0));
project_text(board, 12, "Hello", board->text, 1);
board->line(board->canvas, 4, 30, 120, 30, 2, blue);
board->circle(board->canvas, 64, 64, 10, blue);
board->ring(board->canvas, 64, 64, 24, 3, blue);
board->rect(board->canvas, 10, 90, 40, 20, blue);
board->fill_rect(board->canvas, 60, 90, 40, 20, blue);`}</Code>
      </Section>

      <Section title="LED ring, buttons and battery">
        <p>LEDs are numbered 0–7. Changes become visible after <Inline>led_show()</Inline>. Call <Inline>led_release()</Inline> to restore the Console LED design.</p>
        <Code>{`board->led_fill(0, 0, 0);
board->led_set(0, 255, 0, 0);
board->led_set(1, 0, 255, 0);
board->led_show();

if (project_button_pressed(board, PROJECT_BUTTON_PLUS)) {
  // board->button_held_ms[1] is the current hold duration.
}
int battery = board->battery_percent; // -1 when unavailable
int charging = board->battery_charging;`}</Code>
      </Section>

      <Section title="Private SD storage">
        <p>Writable files are sandboxed in <Inline>/projects-data/&lt;project-id&gt;</Inline>. A read or write handles at most 1,024 bytes. Calls return <Inline>-1</Inline> while the card or File Manager is unavailable.</p>
        <Code>{`const char message[] = "hello";
board->storage_write("settings.txt", message, 5, 0); // overwrite
board->storage_write("events.log", message, 5, 1);  // append

char data[64];
int size = board->storage_size("settings.txt");
int read = board->storage_read("settings.txt", 0, data, sizeof(data));
board->storage_mkdir("logs");
board->storage_remove("old.txt");

char name[64]; int is_dir; uint32_t bytes;
for (int i = 0; board->storage_list(i, name, sizeof(name), &is_dir, &bytes) > 0; ++i) {
  // Use name, is_dir and bytes.
}`}</Code>
        <p>Catalog assets remain read-only through <Inline>asset_size</Inline>, <Inline>asset_read</Inline>, <Inline>media_info</Inline> and <Inline>media_draw</Inline>.</p>
      </Section>

      <Section title="Microphones and speaker">
        <p>Audio is signed 16-bit stereo, interleaved left/right. The sample rate is in <Inline>audio_sample_rate</Inline>. Each non-blocking call accepts at most 240 frames.</p>
        <Code>{`int available = board->audio_begin();
int16_t samples[240 * 2];

if (available & PROJECT_AUDIO_MICROPHONES) {
  int frames = board->mic_read(samples, 240);
}
if (available & PROJECT_AUDIO_SPEAKER) {
  board->speaker_enable(1);
  board->speaker_write(samples, 240);
  // board->speaker_enable(0);
}`}</Code>
      </Section>

      <Section title="Wi-Fi scan and HTTP(S)">
        <p>Connection state is available in <Inline>wifi_state</Inline>, <Inline>wifi_ssid</Inline>, <Inline>wifi_ip</Inline> and <Inline>wifi_rssi</Inline>. Scans and HTTP requests run in background tasks.</p>
        <Code>{`board->wifi_scan_start();
int count = board->wifi_scan_count(); // -1 while scanning
char ssid[40]; int rssi, channel, secure;
if (count > 0) {
  board->wifi_scan_result(0, ssid, sizeof(ssid), &rssi, &channel, &secure);
}

static int requested;
if (!requested && board->wifi_state == PROJECT_WIFI_CONNECTED) {
  requested = board->http_get("https://example.com/data.json") == 0;
}
char response[512]; int http_status;
int n = board->http_result(response, sizeof(response), &http_status);
// n == -2: running, -1: failed, >= 0: copied response bytes.
// The complete HTTP response is limited to 8 KB.`}</Code>
      </Section>

      <Section title="BLE scan">
        <p>BLE discovery is asynchronous and does not block display rendering.</p>
        <Code>{`board->ble_scan_start();
int count = board->ble_scan_count(); // -1 while scanning
char address[20], name[40]; int rssi;
if (count > 0) {
  board->ble_scan_result(0, address, sizeof(address), name, sizeof(name), &rssi);
}`}</Code>
      </Section>

      <Section title="Reliability rules">
        <ul className="list-disc space-y-1 pl-5">
          <li>Never use an infinite loop or a long delay inside <Inline>loop()</Inline>.</li>
          <li>Start scans and HTTP once, then poll their result on later frames.</li>
          <li>Check every negative return value; hardware may be busy or unavailable.</li>
          <li>Projects cannot access raw GPIO, saved Wi-Fi passwords, firmware globals or another project’s files.</li>
          <li>These APIs require firmware 1.1.9 or newer and Project ABI 4.</li>
        </ul>
      </Section>
    </div>
  );
}

function Section({ title, children }: { title: string; children: ReactNode }) {
  return <section className="space-y-3"><h3 className="text-sm font-semibold text-zinc-950 dark:text-white">{title}</h3>{children}</section>;
}

function Code({ children }: { children: string }) {
  return <pre className={codeClass}><code>{children}</code></pre>;
}

function Inline({ children }: { children: ReactNode }) {
  return <code className="rounded bg-zinc-100 px-1 py-0.5 text-xs text-zinc-900 dark:bg-zinc-800 dark:text-zinc-100">{children}</code>;
}

