import { stream } from "@wendoo/core";
import {
  extractNumberValue,
  List,
  mkClosedStructValue,
  mkNumberValue,
  type StructValue,
  type Value,
} from "@wendoo/core/app";
import { bufferByteAt, bufferLength, isBufferValue, isStructValue, mkBufferValue } from "@wendoo/core/runtime";
import { ImageField, WODAL_SHARED_TYPE_IDS } from "./shared-type-ids";

/** Side length, in pixels, of the square grid a pixel-digit string encodes. */
const kGridSize = 5;

/** Number of pixels a pixel-digit string carries. */
const kGridPixelCount = kGridSize * kGridSize;

/** Brightness step between adjacent pixel-digit levels: level 0 is 0 and level 15 is 255. */
const kLevelStep = 17;

/** A well-formed pixel-digit string: one lowercase hex brightness digit per pixel. */
const kGridDigitsPattern = new RegExp(`^[0-9a-f]{${kGridPixelCount}}$`);

/**
 * Builds an `Image` struct value from its dimensions and pixel bytes. The
 * fields are stored in {@link ImageField} slot order: `width` and `height` as
 * numbers and `pixels` as a `Buffer` holding one brightness byte per pixel in
 * row-major order (each byte stored as its low 8 bits).
 *
 * @param width - Image width in columns.
 * @param height - Image height in rows.
 * @param pixels - `width * height` brightness bytes, row-major.
 */
export function mkImageStructValue(width: number, height: number, pixels: readonly number[]): StructValue {
  const slots: Value[] = [];
  slots[ImageField.Width] = mkNumberValue(width);
  slots[ImageField.Height] = mkNumberValue(height);
  slots[ImageField.Pixels] = mkBufferValue(stream.byteArrayFromUint8Array(new Uint8Array(pixels)));
  return mkClosedStructValue(WODAL_SHARED_TYPE_IDS.Image, List.from(slots));
}

/**
 * True when `value` is an `Image` struct value: the wodal-shared `Image`
 * struct type carrying numeric `width` and `height` fields and a `pixels`
 * buffer.
 */
export function isImageStructValue(value: unknown): value is StructValue {
  const candidate = value as Value | undefined;
  if (!isStructValue(candidate) || candidate.typeId !== WODAL_SHARED_TYPE_IDS.Image || candidate.v === undefined) {
    return false;
  }
  return (
    extractNumberValue(candidate.v.at(ImageField.Width)) !== undefined &&
    extractNumberValue(candidate.v.at(ImageField.Height)) !== undefined &&
    isBufferValue(candidate.v.at(ImageField.Pixels))
  );
}

/**
 * The brightness byte pixel-digit level `level` stands for: level 0 is unlit
 * and level 15 is 255.
 *
 * @param level - Brightness level, 0 to 15, as one hex digit names it.
 */
export function imageLevelBrightness(level: number): number {
  return level * kLevelStep;
}

/**
 * True when `text` is a well-formed pixel-digit string: 25 lowercase hex
 * brightness digits, one per pixel of a 5x5 grid, row-major.
 */
export function isImageGridDigits(text: string): boolean {
  return kGridDigitsPattern.test(text);
}

/**
 * The `Image` struct value a pixel-digit string names: a 5x5 image whose every
 * pixel takes the brightness of its own digit, as {@link imageLevelBrightness}
 * scales it. Returns `undefined` for text that is not a well-formed pixel-digit
 * string.
 *
 * @param digits - 25 lowercase hex brightness digits, row-major.
 */
export function imageValueFromDigits(digits: string): StructValue | undefined {
  if (!isImageGridDigits(digits)) return undefined;
  const bytes = [...digits].map((digit) => imageLevelBrightness(Number.parseInt(digit, 16)));
  return mkImageStructValue(kGridSize, kGridSize, bytes);
}

/**
 * The grid an `Image` struct value fills: 25 brightness bytes (0-255),
 * row-major, read from the image's top-left 5x5 region. A row or column the
 * image does not reach, and a pixel its buffer does not hold, reads as 0.
 * Returns `undefined` for a value that is not an `Image` struct value.
 *
 * @param value - The value to read, as carried by an image literal tile.
 */
export function imageGridBytes(value: unknown): number[] | undefined {
  if (!isImageStructValue(value)) return undefined;
  const slots = value.v;
  const width = extractNumberValue(slots?.at(ImageField.Width)) ?? 0;
  const height = extractNumberValue(slots?.at(ImageField.Height)) ?? 0;
  const pixels = slots?.at(ImageField.Pixels);
  if (!isBufferValue(pixels)) return undefined;
  const pixelCount = bufferLength(pixels);
  const bytes: number[] = [];
  for (let row = 0; row < kGridSize; row++) {
    for (let col = 0; col < kGridSize; col++) {
      const index = row * width + col;
      const inside = row < height && col < width && index < pixelCount;
      bytes.push(inside ? (bufferByteAt(pixels, index) ?? 0) : 0);
    }
  }
  return bytes;
}

/**
 * The pixel-digit string an `Image` struct value reads as: one lowercase hex
 * digit per pixel of the grid {@link imageGridBytes} fills, each brightness
 * rounded to the nearest level. Returns `undefined` for a value that is not an
 * `Image` struct value.
 *
 * @param value - The value to read, as carried by an image literal tile.
 */
export function imageDigitsFromValue(value: unknown): string | undefined {
  const bytes = imageGridBytes(value);
  if (bytes === undefined) return undefined;
  return bytes.map((brightness) => Math.round(brightness / kLevelStep).toString(16)).join("");
}
