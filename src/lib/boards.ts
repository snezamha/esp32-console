import manifest from "../../firmware/manifest.json";

export type FirmwareVersion = {
  version: string;
  builtAt: string;
  size: number;
  sha256: string;
  /** Application-only image for over-the-air updates (`<version>.app.bin`). */
  app?: { size: number; sha256: string; md5: string };
};

export type FlashRegion = {
  name: string;
  offset: number;
  size: number;
};

export type Board = {
  id: string;
  name: string;
  vendor: string;
  /** Chip name as reported by esptool-js (`loader.chip.CHIP_NAME`). */
  chip: string;
  /** Minimum flash size required by the image, in MB. */
  flashSizeMb: number;
  /** Flash regions left untouched when flashing (e.g. saved settings). */
  preserve?: FlashRegion[];
  /** Newest first. Built by `pnpm firmware:build` from `firmware/<id>/`. */
  versions: FirmwareVersion[];
};

export const BOARDS: Board[] = manifest.boards;

/** Flashable image (written at 0x0), served from `public/firmware/`. */
export function firmwareUrl(board: Board, version: FirmwareVersion) {
  return `/firmware/${board.id}/${version.version}.bin`;
}
