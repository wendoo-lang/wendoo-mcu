import assert from "node:assert/strict";
import { describe, test } from "node:test";
import { extractNumberValue } from "@wendoo/core/app";
import { bufferToHex, isBufferValue } from "@wendoo/core/runtime";
import { isImageStructValue, mkImageStructValue } from "./image-value";
import { ImageField, WODAL_SHARED_TYPE_IDS } from "./shared-type-ids";

/** Brightness bytes of a `width` x `height` image, cycling the 16 brightness levels. */
function rampBytes(width: number, height: number): number[] {
  const bytes: number[] = [];
  for (let i = 0; i < width * height; i++) {
    bytes.push((i % 16) * 17);
  }
  return bytes;
}

describe("mkImageStructValue", () => {
  test("stores the dimensions and pixel bytes in the Image field slot order", () => {
    const bytes = rampBytes(5, 3);

    const value = mkImageStructValue(5, 3, bytes);

    assert.equal(value.typeId, WODAL_SHARED_TYPE_IDS.Image);
    assert.equal(extractNumberValue(value.v?.at(ImageField.Width)), 5);
    assert.equal(extractNumberValue(value.v?.at(ImageField.Height)), 3);
    const pixels = value.v?.at(ImageField.Pixels);
    assert.ok(isBufferValue(pixels));
    assert.equal(bufferToHex(pixels), bytes.map((byte) => byte.toString(16).padStart(2, "0")).join(""));
  });
});

describe("isImageStructValue", () => {
  test("accepts a built Image struct value", () => {
    assert.equal(isImageStructValue(mkImageStructValue(5, 5, rampBytes(5, 5))), true);
  });

  test("rejects values that are not Image structs", () => {
    assert.equal(isImageStructValue(undefined), false);
    assert.equal(isImageStructValue("0123456789abcdef012345678"), false);
    assert.equal(isImageStructValue(42), false);
    assert.equal(isImageStructValue(true), false);
  });
});
