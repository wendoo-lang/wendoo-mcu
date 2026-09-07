import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { describe, test } from "node:test";
import { fileURLToPath } from "node:url";
import { renderToStaticMarkup } from "react-dom/server";
import { imageLiteralType, kImagePixelsFieldKey } from "./brain/image-literal-type";
import { deviceLedGlow } from "./led-palette";

/** A drawn grid whose first row runs level 0, 8, f, so one row holds both extremes. */
const kProbeDigits = ["08f00", "00000", "00000", "00000", "00000"].join("");

/** The brightness byte of hex level `8`, the mid-scale probe. */
const kMidBrightness = 8 * 17;

/** The MicrobitDevice component's source text. */
function deviceSource(): string {
  return readFileSync(fileURLToPath(new URL("./components/MicrobitDevice.tsx", import.meta.url)), "utf8");
}

/** The LED elements a placed image tile's preview draws, in row-major order. */
function editorPreviewLeds(digits: string): string[] {
  const markup = renderToStaticMarkup(
    imageLiteralType.renderValue?.(imageLiteralType.parseValue({ [kImagePixelsFieldKey]: digits }))
  );
  return [...markup.matchAll(/<span[^>]*data-testid="image-literal-pixel"[^>]*><\/span>/g)].map((match) => match[0]);
}

/** The LED elements the create-literal dialog's grid draws, in row-major order. */
function editorDialogLeds(digits: string): string[] {
  const markup = renderToStaticMarkup(
    imageLiteralType.renderInputFields(
      { [kImagePixelsFieldKey]: digits },
      () => undefined,
      () => undefined
    )
  );
  return [...markup.matchAll(/<span style="[^"]*"><\/span>/g)].map((match) => match[0]);
}

/** The blur and spread, in CSS pixels, and the alpha a `box-shadow` carries. */
function glowOf(shadow: string): { blur: number; spread: number; alpha: number } {
  const match = shadow.match(/0 0 ([\d.]+)px ([\d.]+)px rgba\(239, 68, 68, ([\d.]+)\)/);
  assert.ok(match, `expected a glow in ${shadow}`);
  return {
    blur: Number.parseFloat(match[1]),
    spread: Number.parseFloat(match[2]),
    alpha: Number.parseFloat(match[3]),
  };
}

describe("the halo a simulated display LED casts", () => {
  test("scales its blur and alpha with the brightness, at a fixed spread", () => {
    const mid = glowOf(deviceLedGlow(kMidBrightness) ?? "");
    const full = glowOf(deviceLedGlow(255) ?? "");
    assert.ok(mid.blur > 0, `expected a positive mid-brightness blur, got ${mid.blur}`);
    assert.ok(mid.alpha > 0, `expected a positive mid-brightness alpha, got ${mid.alpha}`);
    assert.ok(full.blur > mid.blur, `blur must rise with brightness: ${full.blur} vs ${mid.blur}`);
    assert.ok(full.alpha > mid.alpha, `alpha must rise with brightness: ${full.alpha} vs ${mid.alpha}`);
    assert.equal(full.spread, mid.spread);
  });

  test("is absent from an unlit LED", () => {
    assert.equal(deviceLedGlow(0), undefined);
  });

  test("stays weaker than an image editor LED's at the same brightness", () => {
    const device = glowOf(deviceLedGlow(255) ?? "");
    const preview = glowOf(editorPreviewLeds(kProbeDigits)[2]);
    const dialog = glowOf(editorDialogLeds(kProbeDigits)[2]);

    for (const editor of [preview, dialog]) {
      assert.ok(device.blur < editor.blur, `device blur ${device.blur} vs editor ${editor.blur}`);
      assert.ok(device.alpha < editor.alpha, `device alpha ${device.alpha} vs editor ${editor.alpha}`);
      assert.ok(device.spread <= editor.spread, `device spread ${device.spread} vs editor ${editor.spread}`);
    }
  });

  test("is drawn by the display's own LED cells", () => {
    const source = deviceSource();
    const cell = source.match(/<div[\s\S]*?data-testid="led"[\s\S]*?\/>/);
    assert.ok(cell, "expected an LED cell element");
    assert.match(cell[0], /boxShadow: deviceLedGlow\(brightness\)/);
  });
});
