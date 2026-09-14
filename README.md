# ESP32 Console

Mobile-first web flasher for ESP32 boards. Built with Next.js, Headless UI and [esptool-js](https://github.com/espressif/esptool-js).

Everything is self-contained: the Arduino firmware source lives in `firmware/`, and the compiled images are served from `public/firmware/`. Nothing is fetched from the internet at runtime.

- Detects ESP32 boards over USB (Web Serial; WebUSB polyfill on Android)
- Reads chip, flash size and MAC, and blocks incompatible firmware
- Groups built-in firmware by manufacturer, board model and version; verifies SHA-256 before flashing
- Flashes a local `.bin` file at a configurable address, with flash-capacity and address-alignment checks
- Keeps the timestamped connection and flashing log open until the user closes it
- **Devices** tab: link boards with the 6-digit code on their screen and configure them remotely

## Boards

| Board | Chip | Flash | Sketch |
| --- | --- | --- | --- |
| Waveshare ESP32-S3-LCD-0.85 | ESP32-S3 | 8MB | `firmware/esp32-s3-lcd-0.85` |

## Web app

```bash
pnpm install
cp .env.example .env.local   # fill in DATABASE_URL, AUTH_SECRET, AUTH_GOOGLE_ID/SECRET — see Deploying
pnpm db:push                 # creates the Devices/Pairings tables
pnpm dev
```

Open http://localhost:3000 in Chrome or Edge. Web Serial needs a secure context (HTTPS or localhost). The Flash firmware tab works without any of the above; only the Devices tab needs a database and Google sign-in configured.

## Devices

Sign in with Google on the **Devices** tab, then link a board:

1. Once a board is on Wi-Fi it long-polls the console (`POST /api/device/sync`, `src/services/console_client.*` in the firmware) and holds each request open until there is something to deliver, so changes reach the board in about a second instead of waiting for the next poll.
2. Unlinked, it receives a 6-digit code and shows it on the home screen and under **Menu → Console**. Codes expire after 10 minutes and are replaced automatically.
3. **Devices → Add Device** with that code links the board to your account. The board stores its token and the code disappears.
4. **Configure** changes display, sound, LED, Bluetooth, clock and time zone settings; edits reach an online board in about a second. Changes made on the board itself flow back to the console the same way.
5. **Device menu** (⋯) sends one-off commands: **Identify** (beep + LED flash), **Send message** (shown on screen), **Restart**, **Power off**; **Details** opens live battery/signal/memory charts, an on-demand **hardware check**, saved **Wi-Fi networks** (a backup network the board switches to if the main one drops for 15 s), **Firmware** (over-the-air updates — see below), and an **Activity** log of every command sent.
6. **Remove** in the console, **Menu → Console → Unlink** on the board, or serial `console unlink` drops the link; an online board shows a new code within a second.

The board needs the console's address, which must be reachable from the board (not `localhost`):

- build it in: `CONSOLE_URL=https://console.example.com pnpm firmware:build`
- or over USB: **Add Device → The device shows no code? → Send address over USB**
- or serial: `console https://console.example.com` (`console off` disables it, `console insecure on` skips the TLS certificate check for self-signed servers)

## Over-the-air updates

`pnpm firmware:build` also writes `<version>.app.bin`, an application-only image the board can fetch over Wi-Fi (the partition table reserves two OTA app slots). From a device's **Details → Firmware** tab, click **Update** to push the newest build; the board downloads it in the background, shows progress in the console, verifies it (size and MD5), and restarts into it. If the download or verification fails, the board keeps running its current firmware. Settings and the console link survive an update.

## Accounts and data

Sign-in is Google only (`src/auth.ts`, [Auth.js](https://authjs.dev) v5) — there is no username/password. Devices belong to the signed-in Google account (`owner` = the account's stable id) and are stored in Postgres via [Prisma](https://www.prisma.io) (`prisma/schema.prisma`: `Device`, `Pairing`). Board check-ins hold their HTTP request open for up to a few seconds waiting for a database change (see `src/lib/device-store.ts`) rather than relying on an in-process event bus, so this works correctly across Vercel's multiple serverless instances — there is no server-side state outside the database.

HTTPS servers are verified against the certificate bundle built into the ESP32 core; a self-signed server needs `console insecure on` on the board (or the toggle in **Menu → Console**), which skips that check.

## Deploying (Vercel + Neon)

1. **Database.** Create a project at [neon.tech](https://neon.tech), then copy its **pooled** connection string (hostname contains `-pooler`) into `DATABASE_URL`.
2. **Google sign-in.** In the [Google Cloud Console](https://console.cloud.google.com/apis/credentials), create an OAuth client (type: Web application) with an authorized redirect URI of `https://<your-domain>/api/auth/callback/google`. Put the client id/secret into `AUTH_GOOGLE_ID` / `AUTH_GOOGLE_SECRET`.
3. **Session secret.** Generate one with `openssl rand -hex 32` and put it in `AUTH_SECRET`.
4. **Push to GitHub**, then import the repo in Vercel (New Project → pick the repo). It auto-detects Next.js; no build settings need changing.
5. Add the three variables above under Project Settings → Environment Variables (Production and Preview), then deploy.
6. **Create the tables once**, from your machine, pointed at the same database: `DATABASE_URL="<the pooled string>" pnpm db:push`.
7. Point boards at the deployed URL: `CONSOLE_URL=https://<your-domain> pnpm firmware:build`, or set it later over USB/serial (see **Devices** above).

`prisma generate` runs automatically after `pnpm install` (the `postinstall` script), which Vercel's build also triggers — no extra build step needed. Each API route declares `export const maxDuration = 10` to fit Vercel's default (Hobby-plan) function limit; on a plan with a longer limit, raising that value (and the matching `MAX_WAIT_S`/`WAIT_MS` constants next to it) shortens the delay before a board or the panel notices a change.

## Custom firmware

Select **Upload .bin file** as the firmware source and choose a local file. The file stays in your browser. Use a binary built for the connected board and its documented flash address: merged images typically use `0x0`, while application-only images often use `0x10000` and require an existing matching bootloader and partition table. Custom flashing overwrites the sectors covered by the image; it does not preserve the built-in firmware’s NVS regions. **Erase flash first** clears the entire flash, including bootloader, partition table and saved settings.

## Firmware

`firmware/esp32-s3-lcd-0.85/` is a hardware check. Pins live in `config.h`; each peripheral has its own driver so later firmware can reuse them.

| Check | Driver | What it verifies |
| --- | --- | --- |
| Battery | `src/board/power_manager.h` | Calibrated ADC voltage, charge pin, power latch |
| Memory | — | PSRAM size and 1 MB read/write, flash size |
| Buttons | `src/common/button.*` | Power, Vol+, Vol- each pressed once |
| LED | `src/led/led_ring.*` | 8 × WS2812: red, green, blue, white, chase |
| SD card | `src/storage/sd_card.h` | SDMMC 4-bit mount, write/read/verify a file |
| Codec | `src/audio/es8311.*`, `es7210.*` | Both codecs answer on I2C and initialise |
| Mic | `src/audio/box_audio_codec.*` | MIC1/MIC2 noise floor over I2S |
| Speaker | same | 1 kHz tone, measured back through the microphones |
| Wi-Fi | `src/net/wireless.*` | Station scan |
| BLE | same | 3 s active scan |

Screen (`src/display/lcd_display.*`): top bar with Wi-Fi (signal bars) and Bluetooth icons, clock, status or notification text, mute and battery icons, low battery popup, light/dark theme.

Buttons: Power click opens the menu, double click toggles the backlight, hold powers off (hold time is a setting; a ring shows the progress and releasing early cancels). Vol+ / Vol- change volume by 10 (hold: max / mute). Vol+ and Vol- together open the hardware check. A press that wakes the dimmed screen does nothing else.

Menu (`src/ui/menu.*`, built in `src/app.cpp`): Vol+ next, Vol- previous, Power select. Pages slide in from the side, rows have icons, switches and ◂ value ▸ choices, long labels scroll, and the last position on each page is remembered. Adjustable values open an editor card, where Vol+ / Vol- change the value and Power confirms. Factory reset needs Power held for 2 s. The menu closes after 30 s without input.

Feedback: boot splash, charging animation in the battery icon, and optional LED ring cues (glow on press, red pulse on low battery, circling green while charging). Defaults: dark theme, auto sleep 1 minute.

| Page | Items |
| --- | --- |
| Console | Status, Code, Name, Server, Error, Update (while installing), Skip cert check (HTTPS only), Unlink |
| Display | Theme, Brightness, Auto sleep (10s–5m, Never), Auto off on battery (1–10m, Never), Rotate 180 |
| Sound & LED | Volume, Button sound, LED ring, LED feedback, LED brightness, LED color |
| Connectivity | Wi-Fi, Status, Network, IP, Scan, Bluetooth, BT name, Clock, Time zone, Time |
| Device | Battery (voltage, level, charging, show %), SD card, Power hold, About, Restart, Power off, Factory reset |
| HW check | Every check, Check all |

Settings are stored in NVS (`src/services/device_config.*`); the web flasher skips that partition, so settings survive updates.

Wi-Fi setup (`src/services/network.*`, `web_portal.*`): with Wi-Fi on and no working network, the board scans once, then opens an access point named `esp32-s3-lcd-0.85` and shows its address (192.168.4.1) on the home screen. Joining it opens the setup page (captive portal): pick a network, enter its password inline, done. The board has one radio, so — deliberately — it never scans or retries a network on its own while a phone is connected to this access point (either would move the radio off the access point's channel and drop the phone); Rescan needs two taps for the same reason. A failed attempt shows why ("Wrong password", "Network not found"). After connecting, the home screen shows the board's IP and the access point stays up for 90 s so the page can show the result, then closes; it reopens if the network is lost. The page stays available at the board's IP and `http://esp32-s3-lcd-0-85.local`. Bluetooth advertises the same name. Serial alternative: `wifi <ssid> <password>`.

Serial (USB CDC, 115200): `help` lists commands. Every result is a JSON line, e.g. `{"test":"mic","status":"ok","detail":"-66/-65dB",...}`.

Requirements: [arduino-cli](https://arduino.github.io/arduino-cli/) with the `esp32:esp32` core (3.x). No extra libraries.

```bash
arduino-cli core install esp32:esp32
```

Release a new version:

1. Edit the sketch in `firmware/<board>/`
2. Bump `FIRMWARE_VERSION` in `firmware/<board>/version.h`
3. Run `pnpm firmware:build`

The script compiles the sketch, writes `public/firmware/<board>/<version>.bin` (merged image, flashed at `0x0`) and `<version>.app.bin` (for over-the-air updates), and updates `firmware/manifest.json`. Only the newest build is kept — the previous version's files are deleted and the manifest holds a single entry per board, so the site only ever offers the latest firmware.

Add a board by creating `firmware/<board>/` with a sketch of the same name, `version.h` and `board.json`.

## Display projects

Projects are **independent native Xtensa ELF binaries**, uploaded into the board’s flash filesystem. Their implementations live in `projects/weather/main.c` and `projects/analog-clock/main.c`, outside the base firmware. The base firmware contains only the generic Espressif ELF loader and the versioned display API (`src/runtime/project_api.h`). Project binaries are never linked into the firmware.

Update the base firmware to **v1.0.4 or later** once, then use the **Projects** tab to upload Weather or Analog clock. The board downloads the selected file, checks its size and MD5, writes it into an inactive project slot, and activates it after successful loading. The previous project stays installed if download, validation or writing fails. Only one module executes at a time. **Default display** unloads the active project. Installing a project does not replace the base firmware or require a restart.

- **Weather:** temperature and conditions from [Open-Meteo](https://open-meteo.com/), cached for 10 minutes. Coordinates are stored in the console (Berlin by default). Wi-Fi and the console connection are needed for new readings.
- **Analog clock:** synchronized board time and the configured time zone.

All module drawing is clipped below the status bar. The base provides marquee text, drawing primitives, time, and generic project data through the C ABI. The device menu remains available. The selected project and uploaded files survive restart and base firmware updates. The project filesystem occupies the existing `spiffs` partition at `0x670000` (1.5 MB); the web flasher preserves it alongside NVS. No database migration is required.

Build projects independently:

```sh
pnpm projects:build
```

This writes separate `.elf` files into `public/projects/<board>/<project>/<version>.elf` and updates `projects/manifest.json`. It uses the installed ESP32-S3 Arduino compiler; set `PROJECT_CC` to `xtensa-esp32s3-elf-gcc` on other setups. New projects implement `app_main(int argc, char **argv)` using the shared `ProjectFrame` ABI and provide a `project.json` manifest. Modules must have no unresolved imports and return after each frame.

Build the generic base separately with `pnpm firmware:build`. Firmware v1.0.4 retains automatic right-to-left marquee for overflowing text, with pauses at both ends; long notifications stay visible for a full pass.

The loader is vendored from [Espressif’s ELF loader](https://github.com/espressif/esp-iot-solution/tree/6958385313b0e4fc1f3de259b1d677fca7d7d236/components/elf_loader), with pinned provenance and Apache-2.0 license under `src/runtime/elf_loader/`.
