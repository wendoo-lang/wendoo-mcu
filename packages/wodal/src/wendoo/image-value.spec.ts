import assert from "node:assert/strict";
import { describe, test } from "node:test";
import { extractNumberValue, type StructValue } from "@wendoo/core/app";
import { bufferToHex, isBufferValue } from "@wendoo/core/runtime";
import {
  imageDigitsFromValue,
  imageGridBytes,
  imageValueFromDigits,
  isImageGridDigits,
  isImageStructValue,
  mkImageStructValue,
} from "./image-value";
import { ImageField, WODAL_SHARED_TYPE_IDS } from "./shared-type-ids";

/** Brightness bytes of a `width` x `height` image, cycling the 16 brightness levels. */
function rampBytes(width: number, height: number): number[] {
  const bytes: number[] = [];
  for (let i = 0; i < width * height; i++) {
    bytes.push((i % 16) * 17);
  }
  return bytes;
}

/** A drawn grid using every brightness level, so a round trip cannot hide one. */
const kRampDigits = ["0123f", "6789a", "bcdef", "01234", "56789"].join("");

/** The `pixels` buffer of `value`, as lowercase hex bytes. */
function pixelHex(value: unknown): string {
  const pixels = (value as StructValue).v?.at(ImageField.Pixels);
  assert.ok(isBufferValue(pixels));
  return bufferToHex(pixels);
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

describe("the pixel-digit encoding", () => {
  test("accepts 25 lowercase hex digits and nothing else", () => {
    assert.equal(isImageGridDigits(kRampDigits), true);
    assert.equal(isImageGridDigits("0".repeat(25)), true);
    assert.equal(isImageGridDigits("0".repeat(24)), false);
    assert.equal(isImageGridDigits("0".repeat(26)), false);
    assert.equal(isImageGridDigits(kRampDigits.toUpperCase()), false);
    assert.equal(isImageGridDigits(""), false);
    assert.equal(isImageGridDigits("0f0f0 fffff fffff 0fff0 00f00"), false);
  });

  test("spells a 5x5 image whose pixel bytes are the digit levels", () => {
    const value = imageValueFromDigits(kRampDigits);

    assert.ok(value);
    assert.equal(value.typeId, WODAL_SHARED_TYPE_IDS.Image);
    assert.equal(extractNumberValue(value.v?.at(ImageField.Width)), 5);
    assert.equal(extractNumberValue(value.v?.at(ImageField.Height)), 5);
    assert.equal(pixelHex(value), [...kRampDigits].map((digit) => digit + digit).join(""));
  });

  test("round-trips every level through the value it spells", () => {
    assert.equal(imageDigitsFromValue(imageValueFromDigits(kRampDigits)), kRampDigits);
  });

  test("spells nothing from a string outside the encoding", () => {
    assert.equal(imageValueFromDigits("0".repeat(24)), undefined);
    assert.equal(imageValueFromDigits(kRampDigits.toUpperCase()), undefined);
    assert.equal(imageValueFromDigits("5x5-00ff00"), undefined);
  });

  test("reads the top-left grid region of an image of any other size", () => {
    const smaller = mkImageStructValue(
      3,
      3,
      [1, 2, 3, 4, 5, 6, 7, 8, 9].map((level) => level * 17)
    );
    const larger = mkImageStructValue(6, 6, rampBytes(6, 6));

    assert.equal(imageDigitsFromValue(smaller), ["12300", "45600", "78900", "00000", "00000"].join(""));
    assert.equal(imageDigitsFromValue(larger), ["01234", "6789a", "cdef0", "23456", "89abc"].join(""));
  });

  test("reads nothing from a value that is not an Image struct", () => {
    assert.equal(imageGridBytes("5x5-00ff00"), undefined);
    assert.equal(imageDigitsFromValue(undefined), undefined);
  });
});
