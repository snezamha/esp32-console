import { findKnownPort, loadSerialApi } from "@/lib/esp";
import type { FileProgress, SdEntry } from "@/lib/device-types";

/**
 * Direct USB link to a board's serial console for the SD file manager. The firmware's `fs`
 * commands answer at once over the cable, so this skips the console relay (polling, TLS
 * handshakes) entirely. One operation runs at a time; replies are JSON lines starting `{"fs"`.
 */

type FsMessage = { fs: string; op?: string; message?: string; [key: string]: unknown };
type Listener = (message: FsMessage) => void;

const IDLE_TIMEOUT_MS = 10_000;

const encodePath = (path: string) => encodeURIComponent(path);
const normalizeMac = (mac: string) => mac.replace(/[^0-9a-f]/gi, "").toUpperCase();

export class SerialLink {
  private readonly port: SerialPort;
  private reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  private writer: WritableStreamDefaultWriter<Uint8Array> | null = null;
  private listeners = new Set<(line: string) => void>();
  private queue: Promise<unknown> = Promise.resolve();
  private closed = false;
  onClose: (() => void) | null = null;

  private constructor(port: SerialPort) {
    this.port = port;
  }

  /** Opens `port` and checks that the firmware behind it belongs to the board with `mac`. */
  static async open(port: SerialPort, mac: string): Promise<SerialLink> {
    await port.open({ baudRate: 115200, bufferSize: 1 << 16 });
    const link = new SerialLink(port);
    try {
      // The S3's USB-CDC only sends to a host that asserts DTR.
      await port.setSignals?.({ dataTerminalReady: true, requestToSend: false }).catch(() => {});
      link.writer = port.writable!.getWriter();
      void link.readLoop();
      const info = await link.infoLine();
      if (!info.mac) throw new Error("The board on this USB port did not answer. Is the ESP32 Console firmware running?");
      if (normalizeMac(String(info.mac)) !== normalizeMac(mac)) throw new Error("The board on this USB port is a different device.");
      const probe = await link.request("fs info");
      if (probe.fs !== "ok") throw new Error("Update the base firmware to use the file manager over USB.");
      return link;
    } catch (error) {
      await link.close();
      throw error;
    }
  }

  get isOpen() {
    return !this.closed;
  }

  async close() {
    if (this.closed) return;
    this.closed = true;
    await this.reader?.cancel().catch(() => {});
    try { this.writer?.releaseLock(); } catch {}
    await this.port.close().catch(() => {});
    this.onClose?.();
  }

  private async readLoop() {
    const decoder = new TextDecoder();
    let buffer = "";
    try {
      while (!this.closed && this.port.readable) {
        this.reader = this.port.readable.getReader();
        try {
          for (;;) {
            const { value, done } = await this.reader.read();
            if (done) break;
            buffer += decoder.decode(value, { stream: true });
            let newline: number;
            while ((newline = buffer.indexOf("\n")) >= 0) {
              const line = buffer.slice(0, newline).trim();
              buffer = buffer.slice(newline + 1);
              if (line) for (const listener of this.listeners) listener(line);
            }
          }
        } finally {
          this.reader.releaseLock();
        }
      }
    } catch {
      // Unplugged or closed.
    }
    if (!this.closed) void this.close();
  }

  private async write(data: Uint8Array | string) {
    if (this.closed || !this.writer) throw new Error("The USB connection was closed.");
    await this.writer.write(typeof data === "string" ? new TextEncoder().encode(data) : data);
  }

  /** Runs `task` after every earlier operation finished. */
  private exclusive<T>(task: () => Promise<T>): Promise<T> {
    const run = this.queue.then(task, task);
    this.queue = run.catch(() => {});
    return run;
  }

  /** Reads the `info` reply, the one non-`fs` line this link needs. */
  private infoLine() {
    return this.exclusive(() => new Promise<Record<string, unknown>>((resolve) => {
      const listener = (line: string) => {
        if (!line.startsWith("{") || !line.includes('"mac"')) return;
        try {
          done(JSON.parse(line));
        } catch {}
      };
      const done = (value: Record<string, unknown>) => {
        clearTimeout(timer);
        this.listeners.delete(listener);
        resolve(value);
      };
      const timer = setTimeout(() => done({}), 2500);
      this.listeners.add(listener);
      void this.write("\ninfo\n").catch(() => done({}));
    }));
  }

  /**
   * Sends one `fs` command and resolves with its final `ok`/`error` line. `onMessage` sees every
   * other line of the reply; returning a promise from it pauses nothing, it is fired and forgotten.
   */
  private send(line: string, onMessage?: Listener): Promise<FsMessage> {
    return new Promise((resolve, reject) => {
      let timer = setTimeout(() => fail(new Error("The board stopped answering over USB.")), IDLE_TIMEOUT_MS);
      const listener = (text: string) => {
        if (!text.startsWith('{"fs"')) return;
        let message: FsMessage;
        try {
          message = JSON.parse(text);
        } catch {
          return;
        }
        clearTimeout(timer);
        timer = setTimeout(() => fail(new Error("The board stopped answering over USB.")), IDLE_TIMEOUT_MS);
        if (message.fs === "ok" || message.fs === "error") {
          finish();
          resolve(message);
        } else {
          onMessage?.(message);
        }
      };
      const finish = () => {
        clearTimeout(timer);
        this.listeners.delete(listener);
      };
      const fail = (error: Error) => {
        finish();
        reject(error);
      };
      this.listeners.add(listener);
      this.write(`\n${line}\n`).catch(fail);
    });
  }

  private request(line: string, onMessage?: Listener) {
    return this.exclusive(() => this.send(line, onMessage));
  }

  private async command(line: string, onMessage?: Listener) {
    const reply = await this.request(line, onMessage);
    if (reply.fs === "error") throw new Error(reply.message || "The board could not complete the operation.");
    return reply;
  }

  async list(path: string) {
    const entries: SdEntry[] = [];
    const reply = await this.command(`fs ls ${encodePath(path)}`, (message) => {
      if (message.fs === "entry") entries.push({ name: String(message.name), folder: message.dir === true, size: Number(message.size) || 0, modified: (Number(message.mtime) || 0) * 1000 });
    });
    entries.sort((a, b) => Number(b.folder) - Number(a.folder) || a.name.localeCompare(b.name));
    return { entries, truncated: reply.truncated === true, total: Number(reply.total) || 0, used: Number(reply.used) || 0 };
  }

  mkdir(path: string) {
    return this.command(`fs mkdir ${encodePath(path)}`);
  }

  rename(path: string, to: string) {
    return this.command(`fs mv ${encodePath(path)} ${encodePath(to)}`);
  }

  remove(path: string) {
    return this.command(`fs rm ${encodePath(path)}`);
  }

  mount() {
    return this.command("fs mount");
  }

  unmount() {
    return this.command("fs unmount");
  }

  format(onProgress: (progress: FileProgress) => void) {
    return this.command("fs format", (message) => {
      if (message.fs === "progress") onProgress({ done: Number(message.done) || 0, total: Number(message.total) || 0 });
    });
  }

  async download(path: string, onProgress: (progress: FileProgress) => void) {
    const chunks: Uint8Array<ArrayBuffer>[] = [];
    let total = 0;
    let done = 0;
    await this.command(`fs get ${encodePath(path)}`, (message) => {
      if (message.fs === "begin") total = Number(message.size) || 0;
      if (message.fs !== "data") return;
      const bytes = Uint8Array.from(atob(String(message.b64)), (c) => c.charCodeAt(0));
      chunks.push(bytes);
      done += bytes.length;
      onProgress({ done, total });
    });
    return new Blob(chunks);
  }

  /** Streams `data` in the acknowledged blocks the board asks for; it verifies the SHA-256. */
  upload(path: string, data: Uint8Array<ArrayBuffer>, onProgress: (progress: FileProgress) => void) {
    return this.exclusive(async () => {
      const digest = await crypto.subtle.digest("SHA-256", data);
      const sha256 = Array.from(new Uint8Array(digest), (b) => b.toString(16).padStart(2, "0")).join("");
      let offset = 0;
      let block = 0;
      let failure: Error | null = null;
      const sendNext = () => {
        if (offset >= data.length) return;
        const end = Math.min(data.length, offset + block);
        const part = data.subarray(offset, end);
        offset = end;
        this.write(part).catch((error) => { failure = error; });
      };
      const reply = await this.send(`fs put ${encodePath(path)} ${data.length} ${sha256}`, (message) => {
        if (message.fs === "ready") {
          block = Number(message.block) || 0;
          onProgress({ done: 0, total: data.length });
          sendNext();
        } else if (message.fs === "ack") {
          onProgress({ done: Number(message.done) || 0, total: data.length });
          sendNext();
        }
      });
      if (failure) throw failure;
      if (reply.fs === "error") throw new Error(reply.message || "Upload failed.");
      return reply;
    });
  }
}

/**
 * Opens a USB link to the board with `mac`: a port this site was already granted is used silently;
 * with `ask`, the browser's port chooser opens when none matches. Returns null when there is no
 * usable port (and `ask` is false) or Web Serial is unavailable.
 */
export async function connectSerialLink(mac: string, ask: boolean): Promise<SerialLink | null> {
  const api = await loadSerialApi();
  if (!api) {
    if (ask) throw new Error("This browser cannot use USB serial. Use Chrome or Edge on a computer.");
    return null;
  }
  const known = await findKnownPort(api);
  if (known) {
    try {
      return await SerialLink.open(known, mac);
    } catch (error) {
      if (!ask) return null;
      if (!(error instanceof DOMException)) throw error;
    }
  }
  if (!ask) return null;
  const port = await api.requestPort();
  return SerialLink.open(port, mac);
}
