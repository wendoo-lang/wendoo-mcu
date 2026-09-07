import { mkLiteralTileId, type StructValue } from "@wendoo/core/app";
import { mkImageStructValue } from "../../../wendoo/image-value";
import { WODAL_SHARED_TYPE_IDS } from "../../../wendoo/shared-type-ids";
import { MICROBIT_LED_MATRIX_SIZE } from "../constants";

/** Brightness of a lit pixel in the built-in icons (full brightness). */
const LIT = 255;

/**
 * A predefined micro:bit icon: a name (the durable literal-tile discriminator),
 * a display label, and 5 rows of 5 cells of pixel art where `#` is a lit pixel
 * (brightness {@link LIT}) and any other character is off.
 */
export interface BuiltInImageDef {
  /** Durable value label; also the literal tile id discriminator. */
  readonly name: string;

  /** Human-readable tile label. */
  readonly label: string;

  /** 5 rows of 5 cells; `#` is lit, any other character is off. */
  readonly art: readonly string[];
}

/**
 * The micro:bit-v2 built-in icon set, each a 5x5 `Image`. Append-only: add new
 * icons at the end and never rename or repurpose an existing entry, since a
 * brain references a built-in literal by its derived id (see
 * {@link builtInImageTileId}). `happy` is the target's default image, drawn when
 * `draw image` is called without an image argument.
 */
export const BUILT_IN_IMAGES: readonly BuiltInImageDef[] = [
  {
    name: "heart",
    label: "heart",
    art: [
      ".#.#.", //
      "#####", //
      "#####", //
      ".###.", //
      "..#..", //
    ],
  },
  {
    name: "happy",
    label: "happy",
    art: [
      ".....", //
      ".#.#.", //
      ".....", //
      "#...#", //
      ".###.", //
    ],
  },
  {
    name: "sad",
    label: "sad",
    art: [
      ".....", //
      ".#.#.", //
      ".....", //
      ".###.", //
      "#...#", //
    ],
  },
  {
    name: "arrow-north",
    label: "arrow north",
    art: [
      "..#..", //
      ".###.", //
      "#.#.#", //
      "..#..", //
      "..#..", //
    ],
  },
  {
    name: "arrow-south",
    label: "arrow south",
    art: [
      "..#..", //
      "..#..", //
      "#.#.#", //
      ".###.", //
      "..#..", //
    ],
  },
  {
    name: "arrow-east",
    label: "arrow east",
    art: [
      "..#..", //
      "...#.", //
      "#####", //
      "...#.", //
      "..#..", //
    ],
  },
  {
    name: "arrow-west",
    label: "arrow west",
    art: [
      "..#..", //
      ".#...", //
      "#####", //
      ".#...", //
      "..#..", //
    ],
  },
];

/** The name of the built-in drawn when `draw image` omits its image argument. */
export const DEFAULT_BUILT_IN_IMAGE_NAME = "happy";

/** Looks up a built-in by name, or throws when no such built-in exists. */
export function getBuiltInImage(name: string): BuiltInImageDef {
  const def = BUILT_IN_IMAGES.find((image) => image.name === name);
  if (!def) {
    throw new Error(`Unknown built-in image '${name}'`);
  }
  return def;
}

/** Packed brightness bytes of a built-in, row-major, one byte per pixel. */
export function builtInImageBytes(def: BuiltInImageDef): number[] {
  const bytes: number[] = [];
  for (const row of def.art) {
    for (const cell of row) {
      bytes.push(cell === "#" ? LIT : 0);
    }
  }
  return bytes;
}

/** Hex of {@link builtInImageBytes} (two lowercase digits per byte). */
export function builtInImageHex(def: BuiltInImageDef): string {
  return builtInImageBytes(def)
    .map((byte) => byte.toString(16).padStart(2, "0"))
    .join("");
}

/**
 * The clipped draw frame of a built-in: its packed brightness bytes and its
 * 5x5 dimensions, ready to paste onto the display.
 */
export function builtInImageFrame(def: BuiltInImageDef): { frame: number[]; width: number; height: number } {
  return { frame: builtInImageBytes(def), width: MICROBIT_LED_MATRIX_SIZE, height: MICROBIT_LED_MATRIX_SIZE };
}

/**
 * The baked `Image` struct value of a built-in: `{ width, height, pixels }` with
 * the 5x5 dimensions and a `Buffer` of the packed brightness bytes. This is the
 * value a surface-1 built-in literal tile carries and bakes into a program.
 */
export function builtInImageStructValue(def: BuiltInImageDef): StructValue {
  return mkImageStructValue(MICROBIT_LED_MATRIX_SIZE, MICROBIT_LED_MATRIX_SIZE, builtInImageBytes(def));
}

/** The derived literal tile id of a built-in image's surface-1 tile. */
export function builtInImageTileId(def: BuiltInImageDef): string {
  return mkLiteralTileId(WODAL_SHARED_TYPE_IDS.Image, def.name);
}
