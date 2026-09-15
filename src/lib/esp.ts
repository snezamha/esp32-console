import type { ESPLoader, Transport } from "esptool-js";

/** USB vendors found on ESP32 dev boards (native USB and common bridges). */
export const USB_FILTERS: SerialPortFilter[] = [
  { usbVendorId: 0x303a }, // Espressif native USB (S2/S3/C3/C6…)
  { usbVendorId: 0x10c4 }, // Silicon Labs CP210x
  { usbVendorId: 0x1a86 }, // WCH CH340 / CH343
  { usbVendorId: 0x0403 }, // FTDI
];

const KNOWN_VENDORS = new Set(USB_FILTERS.map((f) => f.usbVendorId));

export type SerialApi = {
  kind: "native" | "webusb";
  requestPort(): Promise<SerialPort>;
  getPorts(): Promise<SerialPort[]>;
  /** Subscribe to USB plug events. Returns an unsubscribe function. */
  onChange(listener: () => void): () => void;
};

/**
 * Web Serial on desktop Chromium; WebUSB polyfill on Android Chrome
 * (works with native-USB boards such as the ESP32-S3).
 */
export async function loadSerialApi(): Promise<SerialApi | null> {
  if (typeof navigator === "undefined") return null;
  const nav = navigator as Navigator & { serial?: Serial; usb?: USB };

  if (nav.serial) {
    const serial = nav.serial;
    return {
      kind: "native",
      requestPort: () => serial.requestPort({ filters: USB_FILTERS }),
      getPorts: () => serial.getPorts(),
      onChange(listener) {
        serial.addEventListener("connect", listener);
        serial.addEventListener("disconnect", listener);
        return () => {
          serial.removeEventListener("connect", listener);
          serial.removeEventListener("disconnect", listener);
        };
      },
    };
  }

  if (nav.usb) {
    const usb = nav.usb;
    const { serial } = await import("web-serial-polyfill");
    return {
      kind: "webusb",
      requestPort: async () =>
        (await serial.requestPort({ filters: USB_FILTERS })) as unknown as SerialPort,
      getPorts: async () => (await serial.getPorts()) as unknown as SerialPort[],
      onChange(listener) {
        usb.addEventListener("connect", listener);
        usb.addEventListener("disconnect", listener);
        return () => {
          usb.removeEventListener("connect", listener);
          usb.removeEventListener("disconnect", listener);
        };
      },
    };
  }

  return null;
}

/**
 * Closes every USB serial port this origin was granted, releasing any this page (or another tab,
 * or a stale connection after a crash/reload) left open. A "Failed to open serial port" error
 * usually means exactly that — the OS only lets one reader hold a port at a time. This cannot
 * release a port held by a program outside the browser (Arduino's Serial Monitor, `screen`, …);
 * the browser has no permission to close another process's handle, so that still needs quitting
 * the other program by hand. Returns how many ports were actually open and got closed.
 */
export async function releaseHeldPorts(api: SerialApi): Promise<number> {
  const ports = await api.getPorts();
  let released = 0;
  for (const port of ports) {
    try {
      await port.close();
      released++;
    } catch {
      // Already closed, or held by a process outside the browser — nothing else to try here.
    }
  }
  return released;
}

export async function findKnownPort(api: SerialApi) {
  const ports = await api.getPorts();
  return (
    ports.find((p) => KNOWN_VENDORS.has(p.getInfo().usbVendorId)) ?? null
  );
}

export type ChipInfo = {
  chip: string;
  description: string;
  mac: string;
  flashSize: string;
  flashSizeMb: number;
};

export type Session = {
  loader: ESPLoader;
  transport: Transport;
  info: ChipInfo;
};

export type Logger = (line: string) => void;

function parseFlashSizeMb(size: string) {
  const match = /^(\d+)(KB|MB)$/.exec(size);
  if (!match) return 0;
  return match[2] === "MB" ? Number(match[1]) : Number(match[1]) / 1024;
}

/** Puts the board into its ROM bootloader, loads the stub and reads chip info. */
export async function connect(port: SerialPort, log: Logger): Promise<Session> {
  const { ESPLoader, Transport } = await import("esptool-js");
  const transport = new Transport(port, false);
  const loader = new ESPLoader({
    transport,
    baudrate: 921600,
    terminal: {
      clean() {},
      writeLine: (data) => log(data),
      write: (data) => log(data),
    },
  });

  try {
    const description = await loader.main("default_reset");
    const flashSize = await loader.detectFlashSize();
    const mac = await loader.chip.readMac(loader);
    return {
      loader,
      transport,
      info: {
        chip: loader.chip.CHIP_NAME,
        description,
        mac,
        flashSize,
        flashSizeMb: parseFlashSizeMb(flashSize),
      },
    };
  } catch (error) {
    await transport.disconnect().catch(() => {});
    throw error;
  }
}

/**
 * Splits a merged image (written at 0x0) into parts that skip the preserved regions,
 * so flashing does not erase them.
 */
export function splitImage(image: Uint8Array, preserve: { offset: number; size: number }[] = []) {
  const parts: { data: Uint8Array; address: number }[] = [];
  const holes = [...preserve].sort((a, b) => a.offset - b.offset);
  let start = 0;
  for (const hole of holes) {
    if (hole.offset > start) {
      parts.push({ data: image.subarray(start, Math.min(hole.offset, image.length)), address: start });
    }
    start = Math.max(start, hole.offset + hole.size);
  }
  if (start < image.length) parts.push({ data: image.subarray(start), address: start });
  return parts.filter((p) => p.data.length > 0);
}

export async function flash(
  session: Session,
  image: Uint8Array,
  options: {
    eraseAll: boolean;
    address?: number;
    preserve?: { offset: number; size: number }[];
    onProgress: (ratio: number) => void;
  },
) {
  // A full erase clears everything anyway, so there is nothing to preserve.
  const address = options.address ?? 0;
  if (!Number.isSafeInteger(address) || address < 0 || address % 4096 !== 0) {
    throw new Error("Flash address must be a non-negative multiple of 0x1000.");
  }
  if (!image.length) throw new Error("Firmware file is empty.");
  if (session.info.flashSizeMb && address + image.length > session.info.flashSizeMb * 1024 * 1024) {
    throw new Error("Firmware exceeds the connected board’s flash capacity.");
  }
  const parts = options.eraseAll || address !== 0
    ? [{ data: image, address }]
    : splitImage(image, options.preserve);
  const total = parts.reduce((sum, p) => sum + p.data.length, 0);
  const done = parts.map(() => 0);

  await session.loader.writeFlash({
    fileArray: parts,
    flashMode: "keep",
    flashFreq: "keep",
    flashSize: "keep",
    eraseAll: options.eraseAll,
    compress: true,
    reportProgress: (index, written, partTotal) => {
      done[index] = partTotal ? (written / partTotal) * parts[index].data.length : 0;
      options.onProgress(total ? done.reduce((a, b) => a + b, 0) / total : 0);
    },
  });
}

/** Resets the chip into the application and releases the port. */
export async function release(session: Session) {
  const { transport } = session;
  try {
    await transport.setDTR(false);
    await transport.setRTS(true);
    await new Promise((r) => setTimeout(r, 100));
    await transport.setRTS(false);
  } catch {
    // Board may already be gone.
  }
  await transport.disconnect().catch(() => {});
}

/** Loads a bundled firmware image with progress and verifies its SHA-256. */
export async function loadFirmware(
  url: string,
  sha256: string,
  onProgress: (ratio: number) => void,
) {
  const res = await fetch(url);
  if (!res.ok || !res.body) throw new Error(`Firmware file missing (${res.status})`);
  const total = Number(res.headers.get("Content-Length")) || 0;
  const reader = res.body.getReader();
  const chunks: Uint8Array[] = [];
  let received = 0;

  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(value);
    received += value.byteLength;
    if (total) onProgress(received / total);
  }

  const image = new Uint8Array(received);
  let offset = 0;
  for (const chunk of chunks) {
    image.set(chunk, offset);
    offset += chunk.byteLength;
  }

  const digest = await crypto.subtle.digest("SHA-256", image);
  const hex = Array.from(new Uint8Array(digest), (b) => b.toString(16).padStart(2, "0")).join("");
  if (hex !== sha256) throw new Error("Firmware checksum mismatch. Rebuild the firmware.");

  onProgress(1);
  return image;
}

export function errorMessage(error: unknown) {
  if (error instanceof Error) return error.message;
  return String(error);
}

/**
 * Sends one line to the firmware's serial console (115200 baud) and returns the reply lines
 * received within `waitMs`. The port must be closed; it is closed again afterwards.
 */
export async function sendSerialCommand(port: SerialPort, line: string, waitMs = 1500) {
  await port.open({ baudRate: 115200 });
  const lines: string[] = [];
  try {
    const writer = port.writable!.getWriter();
    await writer.write(new TextEncoder().encode(`\n${line}\n`));
    writer.releaseLock();

    const reader = port.readable!.getReader();
    const timer = setTimeout(() => void reader.cancel().catch(() => {}), waitMs);
    const decoder = new TextDecoder();
    let buffer = "";
    try {
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        buffer += decoder.decode(value, { stream: true });
      }
    } finally {
      clearTimeout(timer);
      reader.releaseLock();
    }
    lines.push(...buffer.split(/\r?\n/).filter(Boolean));
  } finally {
    await port.close().catch(() => {});
  }
  return lines;
}
