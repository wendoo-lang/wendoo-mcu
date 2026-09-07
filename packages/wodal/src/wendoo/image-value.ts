import { stream } from "@wendoo/core";
import { List, mkClosedStructValue, mkNumberValue, type StructValue, type Value } from "@wendoo/core/app";
import { mkBufferValue } from "@wendoo/core/runtime";
import { ImageField, WODAL_SHARED_TYPE_IDS } from "./shared-type-ids";

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
