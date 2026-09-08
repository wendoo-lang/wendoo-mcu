import assert from "node:assert/strict";
import { describe, test } from "node:test";
import { extractNumberValue, isStructValue, mkLiteralTileId, type Value } from "@wendoo/core/app";
import type { BrainTileLiteralDef } from "@wendoo/core/brain/tiles";
import { bufferToHex, isBufferValue } from "@wendoo/core/runtime";
import { ImageField, mkImageStructValue, WODAL_SHARED_TYPE_IDS } from "@wendoo/wodal";
import { createMicroBitV2Environment } from "@wendoo/wodal/targets/microbit-v2";
import { renderToStaticMarkup } from "react-dom/server";
import { buildMicrobitBrainEditorConfig } from "./editor-config";
import { imageLiteralType, kImagePixelsFieldKey, paintGridDigit } from "./image-literal-type";

/** A drawn grid: full-brightness heart, one hex digit per pixel, row-major. */
const kHeartDigits = ["0f0f0", "fffff", "fffff", "0fff0", "00f00"].join("");

/** A drawn grid using every brightness level, so a round trip cannot hide one. */
const kRampDigits = ["0123f", "6789a", "bcdef", "01234", "56789"].join("");

/**
 * The height, in CSS pixels, a placed tile's value box stands at: the minimum
 * every value tile reserves, drawing and text alike. A drawing taller than what
 * the frame leaves inside it is clipped there.
 */
const kValueBoxHeight = 64;

/** Height the value frame takes from what it carries: a 3px border plus 4px of padding per side. */
const kValueFrameHeightInset = 2 * (3 + 4);

/** The height, in CSS pixels, the framed value box leaves the drawing. */
const kPreviewHeightBudget = kValueBoxHeight - kValueFrameHeightInset;

/** The input state a grid's digits travel in. */
function stateOf(digits: string): Record<string, string> {
  return { [kImagePixelsFieldKey]: digits };
}

/** The `pixels` buffer of an `Image` struct value, as lowercase hex bytes. */
function pixelHex(value: unknown): string {
  const struct = value as Value | undefined;
  assert.ok(isStructValue(struct) && struct.v !== undefined, "expected an Image struct value");
  const pixels = struct.v.at(ImageField.Pixels);
  assert.ok(isBufferValue(pixels), "expected a pixels buffer");
  return bufferToHex(pixels);
}

/** The dimensions an `Image` struct value carries, read from its own slots. */
function dimensionsOf(value: unknown): { width: number | undefined; height: number | undefined } {
  const struct = value as Value | undefined;
  assert.ok(isStructValue(struct) && struct.v !== undefined, "expected an Image struct value");
  return {
    width: extractNumberValue(struct.v.at(ImageField.Width)),
    height: extractNumberValue(struct.v.at(ImageField.Height)),
  };
}

/** Each hex digit doubled: the brightness byte of level `n` is `0xnn`. */
function expectedPixelHex(digits: string): string {
  return [...digits].map((digit) => digit + digit).join("");
}

/** The baked value of the shipped built-in image literal tile named `name`. */
function builtInImageValue(name: string): unknown {
  const env = createMicroBitV2Environment();
  const tileId = mkLiteralTileId(WODAL_SHARED_TYPE_IDS.Image, name);
  for (const catalog of env.tileCatalogs()) {
    const tileDef = catalog.get(tileId);
    if (tileDef) {
      return (tileDef as BrainTileLiteralDef).value;
    }
  }
  throw new Error(`no built-in image literal tile ${tileId}`);
}

/** The markup a placed literal of this type draws for `value`. */
function renderedValue(value: unknown): string {
  return renderToStaticMarkup(imageLiteralType.renderValue?.(value));
}

/** The markup the create-literal dialog's fields draw for `state`. */
function renderedFields(state: Record<string, string>): string {
  return renderToStaticMarkup(
    imageLiteralType.renderInputFields(
      state,
      () => undefined,
      () => undefined
    )
  );
}

/** The LED elements of a rendered preview, in row-major order. */
function previewLeds(markup: string): string[] {
  return [...markup.matchAll(/<span[^>]*data-testid="image-literal-pixel"[^>]*><\/span>/g)].map((match) => match[0]);
}

/** The well elements a rendered preview centers its LEDs in, in row-major order. */
function previewWells(markup: string): string[] {
  return [...markup.matchAll(/<span class="flex items-center justify-center" style="[^"]*"><span/g)].map(
    (match) => match[0]
  );
}

/** The opening tag of a rendered preview's grid container. */
function previewContainer(markup: string): string {
  const match = markup.match(/<div[^>]*data-testid="image-literal-preview"[^>]*>/);
  assert.ok(match, "expected a preview grid container");
  return match[0];
}

/** The value, in CSS pixels, that `property` takes in an element's inline style. */
function stylePx(element: string, property: string): number {
  const match = element.match(new RegExp(`(?:^|[";])${property}:(\\d+(?:\\.\\d+)?)px`));
  assert.ok(match, `expected ${property} in ${element}`);
  return Number.parseFloat(match[1]);
}

/** The blur and spread, in CSS pixels, and the alpha the `box-shadow` of `element` carries. */
function glowOf(element: string): { blur: number; spread: number; alpha: number } {
  const match = element.match(/box-shadow:0 0 ([\d.]+)px ([\d.]+)px rgba\(239, 68, 68, ([\d.]+)\)/);
  assert.ok(match, `expected a glow in ${element}`);
  return {
    blur: Number.parseFloat(match[1]),
    spread: Number.parseFloat(match[2]),
    alpha: Number.parseFloat(match[3]),
  };
}

/** The brightness each LED of a rendered preview carries. */
function previewBrightness(markup: string): number[] {
  return previewLeds(markup).map((led) => {
    const match = led.match(/data-brightness="(\d+)"/);
    assert.ok(match, "expected an LED to carry its brightness");
    return Number.parseInt(match[1], 10);
  });
}

describe("the image literal type", () => {
  test("a drawn grid round-trips through the value it names", () => {
    for (const digits of [kHeartDigits, kRampDigits]) {
      const value = imageLiteralType.parseValue(stateOf(digits));
      assert.deepEqual(imageLiteralType.toInputState(value), stateOf(digits), digits);
    }
  });

  test("a drawn grid parses to a 5x5 value whose pixel bytes are the hex-digit scale", () => {
    for (const digits of [kHeartDigits, kRampDigits]) {
      const value = imageLiteralType.parseValue(stateOf(digits));
      assert.deepEqual(dimensionsOf(value), { width: 5, height: 5 }, digits);
      assert.equal(pixelHex(value), expectedPixelHex(digits), digits);
    }
  });

  test("two grids that differ produce values that differ", () => {
    assert.notEqual(
      pixelHex(imageLiteralType.parseValue(stateOf(kHeartDigits))),
      pixelHex(imageLiteralType.parseValue(stateOf(kRampDigits)))
    );
  });

  test("an empty input state names the unlit grid, and a malformed one names nothing", () => {
    assert.equal(imageLiteralType.isValid({}), true);
    assert.equal(pixelHex(imageLiteralType.parseValue({})), "00".repeat(25));
    assert.equal(imageLiteralType.isValid(stateOf("0f0")), false);
    assert.equal(imageLiteralType.parseValue(stateOf("0f0")), undefined);
  });

  test("a value smaller than the grid seeds its top-left region and pads the rest", () => {
    const value = mkImageStructValue(
      3,
      3,
      [1, 2, 3, 4, 5, 6, 7, 8, 9].map((level) => level * 17)
    );
    assert.deepEqual(
      imageLiteralType.toInputState(value),
      stateOf(["12300", "45600", "78900", "00000", "00000"].join(""))
    );
  });

  test("a value larger than the grid seeds its top-left region", () => {
    const pixels = Array.from({ length: 36 }, (_unused, index) => (index % 16) * 17);
    const value = mkImageStructValue(6, 6, pixels);
    assert.deepEqual(
      imageLiteralType.toInputState(value),
      stateOf(["01234", "6789a", "cdef0", "23456", "89abc"].join(""))
    );
  });

  test("a value of another shape seeds no fields and draws no node", () => {
    assert.deepEqual(imageLiteralType.toInputState("5x5-00ff00"), {});
    assert.equal(imageLiteralType.renderValue?.("5x5-00ff00"), undefined);
  });

  test("a tap lights an unlit pixel at the selected level, and any lit pixel goes out", () => {
    const blank = "0".repeat(25);
    const lit = paintGridDigit(blank, 7, 15);

    assert.equal(lit, `${"0".repeat(7)}f${"0".repeat(17)}`);
    assert.equal(paintGridDigit(lit, 7, 15), blank);
    assert.equal(paintGridDigit(lit, 7, 9), blank);
    assert.equal(paintGridDigit(blank, 7, 0), blank);
    assert.equal(paintGridDigit(blank, 3, 9), `${"0".repeat(3)}9${"0".repeat(21)}`);
  });

  test("a built-in image literal draws its baked pixels", () => {
    const markup = renderedValue(builtInImageValue("heart"));
    assert.match(markup, /data-testid="image-literal-preview"/);
    assert.deepEqual(
      previewBrightness(markup),
      [...kHeartDigits].map((digit) => Number.parseInt(digit, 16) * 17)
    );
  });

  test("a drawn image literal draws the pixels it was drawn with", () => {
    const markup = renderedValue(imageLiteralType.parseValue(stateOf(kRampDigits)));
    assert.match(markup, /data-testid="image-literal-preview"/);
    assert.deepEqual(
      previewBrightness(markup),
      [...kRampDigits].map((digit) => Number.parseInt(digit, 16) * 17)
    );
  });
});

describe("the LEDs an image literal is drawn with", () => {
  test("take the display's own fill for their brightness, in the preview and in the editor", () => {
    const leds = previewLeds(renderedValue(imageLiteralType.parseValue(stateOf(kRampDigits))));
    const editorLeds = [...renderedFields(stateOf(kRampDigits)).matchAll(/<span style="[^"]*"><\/span>/g)].map(
      (match) => match[0]
    );

    // The ramp's first row runs level 0, 1, 2, 3, f.
    assert.match(leds[0], /background-color:rgba\(120, 20, 20, 0\.35\)/);
    assert.match(leds[1], /background-color:rgba\(239, 68, 68, 0\.347\)/);
    assert.match(leds[2], /background-color:rgba\(239, 68, 68, 0\.393\)/);
    assert.match(leds[4], /background-color:rgba\(239, 68, 68, 1\.000\)/);
    assert.match(editorLeds[0], /background-color:rgba\(120, 20, 20, 0\.35\)/);
    assert.match(editorLeds[4], /background-color:rgba\(239, 68, 68, 1\.000\)/);
  });

  test("glow only where they are lit, at a halo scaled to the brightness", () => {
    // The ramp's first row runs level 0, 1, 2, 3, f.
    const leds = previewLeds(renderedValue(imageLiteralType.parseValue(stateOf(kRampDigits))));
    const dim = glowOf(leds[1]);
    const full = glowOf(leds[4]);

    assert.doesNotMatch(leds[0], /box-shadow/);
    assert.ok(dim.blur > 0, `expected a positive dim blur, got ${dim.blur}`);
    assert.ok(dim.alpha > 0, `expected a positive dim alpha, got ${dim.alpha}`);
    assert.ok(full.blur > dim.blur, `blur must rise with brightness: ${full.blur} vs ${dim.blur}`);
    assert.ok(full.alpha > dim.alpha, `alpha must rise with brightness: ${full.alpha} vs ${dim.alpha}`);
    assert.ok(full.spread >= dim.spread, `spread must not fall with brightness: ${full.spread} vs ${dim.spread}`);
  });

  test("stand taller than they are wide, as the device's own do", () => {
    const previewLed = previewLeds(renderedValue(imageLiteralType.parseValue(stateOf(kHeartDigits))))[0];
    const editorLed = [...renderedFields(stateOf(kHeartDigits)).matchAll(/<span style="[^"]*"><\/span>/g)][0][0];

    for (const led of [previewLed, editorLed]) {
      assert.ok(
        stylePx(led, "height") > stylePx(led, "width"),
        `expected a portrait LED, got ${stylePx(led, "width")}x${stylePx(led, "height")}`
      );
    }
  });

  test("sit in tap targets the grid editor sizes for a fingertip", () => {
    const markup = renderedFields(stateOf(kHeartDigits));
    const cell = markup.match(/<button[^>]*data-testid="image-literal-cell"[^>]*>/);
    assert.ok(cell);

    assert.equal(stylePx(cell[0], "width"), 40);
    assert.equal(stylePx(cell[0], "height"), 40);
  });
});

describe("the in-tile preview", () => {
  test("stands inside the height the framed value box leaves it, with room to spare", () => {
    const markup = renderedValue(imageLiteralType.parseValue(stateOf(kHeartDigits)));
    const wells = previewWells(markup);
    const container = previewContainer(markup);

    assert.equal(wells.length, 25);
    const gap = stylePx(container, "gap");
    const padding = stylePx(container, "padding");
    const height = 5 * stylePx(wells[0], "height") + 4 * gap + 2 * padding;

    assert.ok(height < kPreviewHeightBudget, `preview stands ${height}px tall, over ${kPreviewHeightBudget}px`);
  });

  test("floats its matrix on the display's own plate, inset from the frame around it", () => {
    const markup = renderedValue(imageLiteralType.parseValue(stateOf(kHeartDigits)));
    const container = previewContainer(markup);

    assert.match(container, /class="[^"]*bg-panel/);
    assert.ok(stylePx(container, "padding") > 0, "the matrix stands inset from the plate's own edge");
  });

  test("stands square and at one size, whatever the image drawn on it", () => {
    const [heart, ramp] = [kHeartDigits, kRampDigits].map((digits) =>
      renderedValue(imageLiteralType.parseValue(stateOf(digits)))
    );

    for (const markup of [heart, ramp]) {
      const well = previewWells(markup)[0];
      assert.equal(stylePx(well, "width"), stylePx(well, "height"));
    }
    assert.equal(stylePx(previewWells(heart)[0], "width"), stylePx(previewWells(ramp)[0], "width"));
    assert.equal(stylePx(previewContainer(heart), "gap"), stylePx(previewContainer(ramp), "gap"));
    assert.equal(stylePx(previewContainer(heart), "padding"), stylePx(previewContainer(ramp), "padding"));
  });

  test("holds each LED whole inside its own well", () => {
    const markup = renderedValue(imageLiteralType.parseValue(stateOf(kHeartDigits)));
    const well = previewWells(markup)[0];
    const led = previewLeds(markup)[0];

    assert.ok(stylePx(led, "width") <= stylePx(well, "width"), "an LED wider than its well is clipped");
    assert.ok(stylePx(led, "height") <= stylePx(well, "height"), "an LED taller than its well is clipped");
  });
});

describe("the grid editor", () => {
  test("stands a cell per pixel, seeded from the input state", () => {
    const markup = renderedFields(stateOf(kRampDigits));
    const cells = [...markup.matchAll(/data-testid="image-literal-cell" data-index="(\d+)" data-level="([0-9a-f])"/g)];

    assert.equal(cells.length, 25);
    assert.equal(cells.map((cell) => cell[2]).join(""), kRampDigits);
  });

  test("picks brightness with one stepped slider over the 16 levels", () => {
    const markup = renderedFields(stateOf(kRampDigits));
    const sliders = [...markup.matchAll(/<input[^>]*data-testid="image-literal-brightness"[^>]*\/?>/g)];

    assert.equal(sliders.length, 1);
    assert.match(sliders[0][0], /type="range"/);
    assert.match(sliders[0][0], /min="0"/);
    assert.match(sliders[0][0], /max="15"/);
    assert.match(sliders[0][0], /step="1"/);
    assert.match(sliders[0][0], /accent-color:var\(--color-primary\)/);
  });

  test("shows the level it is set to on one LED chip", () => {
    const markup = renderedFields(stateOf(kRampDigits));
    const chips = [...markup.matchAll(/data-testid="image-literal-brightness-preview" data-level="([0-9a-f])"/g)];

    assert.equal(chips.length, 1);
    assert.equal(chips[0][1], "f");
  });
});

describe("the brain editor config", () => {
  test("carries this type, its icon, its name and the word its defaults number from", () => {
    const config = buildMicrobitBrainEditorConfig({
      env: createMicroBitV2Environment(),
      resolveVfsAssetUrl: (url) => url,
    });
    const entry = config.customLiteralTypes.find((candidate) => candidate.typeId === WODAL_SHARED_TYPE_IDS.Image);

    assert.equal(entry, imageLiteralType);
    assert.equal(imageLiteralType.nameBase, "image");
    assert.ok(config.dataTypeIcons.get(WODAL_SHARED_TYPE_IDS.Image));
    assert.ok(config.dataTypeNames.get(WODAL_SHARED_TYPE_IDS.Image));
  });
});
