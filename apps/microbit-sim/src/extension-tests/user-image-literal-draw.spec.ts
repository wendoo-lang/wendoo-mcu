/**
 * A drawn image end to end on the simulated device: the literal the image
 * factory mints paints its own pixels through `draw image`, survives the
 * project's save and load carrying its identity, its name and its pixels, and
 * a second literal drawn from the same grid is an image of its own that an
 * edit of the first never reaches.
 */

import assert from "node:assert/strict";
import { describe, test } from "node:test";
import type { IBrainDef, IBrainRuleDef, WendooEnvironment } from "@wendoo/core/app";
import {
  BrainDef,
  BrainTileLiteralDef,
  CoreTypeIds,
  mkActuatorTileId,
  mkParameterTileId,
  mkSensorTileId,
} from "@wendoo/core/app";
import { mkLiteralFactoryTileId } from "@wendoo/core/brain";
import { EditLiteralCommand, encodePersistedBrainJson } from "@wendoo/core/brain/model";
import type { BrainTileFactoryDef } from "@wendoo/core/brain/tiles";
import { manufactureLiteralTile } from "@wendoo/core/brain/tiles";
import { CoreHostActions } from "@wendoo/core/runtime";
import { getWodalDeviceProfile, WODAL_IMAGE_LITERAL_FACTORY_ID, WodalDeviceProfileId } from "@wendoo/wodal";
import {
  createMicroBitV2Environment,
  MicroBitV2HostActions,
  WodalMicroBitV2ParameterId,
} from "@wendoo/wodal/targets/microbit-v2";
import { imageGridBytes, imageLiteralType, kImagePixelsFieldKey } from "../brain/image-literal-type";
import { MicrobitSimulator } from "../services/simulator";

/** Simulated milliseconds each tick advances the device. */
const kTickMs = 16;

/** Namespace the project saves and loads its brains under. */
const kProjectNamespace = "user-image-draw-tests";

/** The grid the first image is drawn on: one hex brightness digit per pixel, row-major. */
const kDrawnDigits = ["12345", "6789a", "bcdef", "13579", "2468a"].join("");

/** The grid the first image is edited to. */
const kEditedDigits = ["2468a", "13579", "bcdef", "6789a", "12345"].join("");

/** The word the first image carries. */
const kFirstName = "image 1";

/** The word the duplicate carries. */
const kSecondName = "image 2";

/** The `Image` struct value the grid editor produces for `digits`. */
function imageValue(digits: string): unknown {
  const value = imageLiteralType.parseValue({ [kImagePixelsFieldKey]: digits });
  assert.ok(value !== undefined, "the grid editor parses the digits it produced");
  return value;
}

/** The brightness byte per pixel `digits` stands for, row-major. */
function expectedBytes(digits: string): number[] {
  return [...digits].map((digit) => Number.parseInt(digit, 16) * 17);
}

/** Mint one image literal of `digits` named `name` into `brainDef`'s own catalog. */
function drawImage(environment: WendooEnvironment, brainDef: IBrainDef, digits: string, name: string) {
  const factory = environment.brainServices.edit.tiles.get(mkLiteralFactoryTileId(WODAL_IMAGE_LITERAL_FACTORY_ID));
  assert.ok(factory, "the image literal factory is registered");
  const literal = manufactureLiteralTile(
    factory as BrainTileFactoryDef,
    brainDef.catalog(),
    imageValue(digits),
    undefined,
    name
  );
  assert.ok(literal, "the image factory mints a literal");
  return literal;
}

/**
 * Fill `rule` with an on-page-entered when() and a `draw image` do() that draws
 * `literal` fire-and-forget: it paints at dispatch and takes no display lease.
 */
function appendDrawRule(
  environment: WendooEnvironment,
  rule: IBrainRuleDef,
  literal: BrainTileLiteralDef,
  zeroDuration: BrainTileLiteralDef
): void {
  const tiles = environment.brainServices.edit.tiles;
  const onPageEntered = tiles.get(mkSensorTileId(CoreHostActions.OnPageEntered.key));
  const drawTile = tiles.get(mkActuatorTileId(MicroBitV2HostActions.DrawImage.key));
  const durationParam = tiles.get(mkParameterTileId(WodalMicroBitV2ParameterId.Duration));
  assert.ok(onPageEntered && drawTile && durationParam, "the draw-image grammar's tiles are registered");
  rule.when().appendTile(onPageEntered);
  rule.do().appendTile(drawTile);
  rule.do().appendTile(literal);
  rule.do().appendTile(durationParam);
  rule.do().appendTile(zeroDuration);
}

/** A zero-duration literal registered in `brainDef`'s catalog, for the draws' duration slot. */
function zeroDurationLiteral(environment: WendooEnvironment, brainDef: IBrainDef): BrainTileLiteralDef {
  const literal = new BrainTileLiteralDef(CoreTypeIds.Number, 0, {}, environment.brainServices);
  brainDef.catalog().registerTileDef(literal);
  return literal;
}

/**
 * Build `brainDef` for the device, flash it onto a fresh simulated instance and
 * run one think, returning every frame the display painted, in paint order.
 * Fails when the brain does not build or load.
 */
function paintedFrames(environment: WendooEnvironment, brainDef: IBrainDef): number[][] {
  const simulator = new MicrobitSimulator(environment);
  const instance = simulator.getInstances()[0];
  assert.ok(instance, "a fresh simulator stands one instance");
  const frames: number[][] = [];
  const display = instance.microbit.display;
  const paintFrame = display.paintFrame.bind(display);
  display.paintFrame = (image) => {
    frames.push([...image.frame]);
    paintFrame(image);
  };
  simulator.flash(
    instance.id,
    {
      brainDef,
      environment,
      deviceProfile: getWodalDeviceProfile(WodalDeviceProfileId.MICROBIT_V2),
    },
    brainDef.id()
  );
  assert.equal(instance.flashState.status, "loaded", JSON.stringify(instance.flashState));
  instance.tick(kTickMs);
  return frames;
}

/** The one rule a fresh brain document already holds. */
function firstRule(brainDef: IBrainDef): IBrainRuleDef {
  const rule = brainDef.pages().get(0)?.children().get(0);
  assert.ok(rule, "a fresh brain document holds one rule");
  return rule;
}

/** A brain whose one rule draws one freshly drawn, named image. */
function oneImageBrain(environment: WendooEnvironment): { brainDef: IBrainDef; literal: BrainTileLiteralDef } {
  const brainDef = BrainDef.emptyBrainDef(environment.brainServices, "drawn image brain");
  const literal = drawImage(environment, brainDef, kDrawnDigits, kFirstName);
  appendDrawRule(environment, firstRule(brainDef), literal, zeroDurationLiteral(environment, brainDef));
  return { brainDef, literal };
}

/** The literal `tileId` names in `brainDef`'s own catalog. */
function literalOf(brainDef: IBrainDef, tileId: string): BrainTileLiteralDef {
  const literal = brainDef.catalog().get(tileId);
  assert.ok(literal, `the document catalog holds ${tileId}`);
  return literal as BrainTileLiteralDef;
}

describe("a drawn image on the simulated device", () => {
  test("paints the pixels the user drew", () => {
    const environment = createMicroBitV2Environment();
    const { brainDef } = oneImageBrain(environment);

    const frames = paintedFrames(environment, brainDef);

    assert.deepEqual(frames, [expectedBytes(kDrawnDigits)]);
  });

  test("paints the edited pixels once the image is edited in place", () => {
    const environment = createMicroBitV2Environment();
    const { brainDef, literal } = oneImageBrain(environment);

    new EditLiteralCommand(brainDef, literal, { value: imageValue(kEditedDigits) }).execute();

    assert.deepEqual(paintedFrames(environment, brainDef), [expectedBytes(kEditedDigits)]);
  });
});

describe("a drawn image saved with the project and loaded again", () => {
  test("comes back under its own id, name and pixels, and still paints them", () => {
    const authoring = createMicroBitV2Environment();
    const { brainDef, literal } = oneImageBrain(authoring);
    const saved = JSON.parse(JSON.stringify(encodePersistedBrainJson(brainDef, kProjectNamespace))) as unknown;

    const loading = createMicroBitV2Environment();
    const reloaded = loading.deserializeBrainJsonFromPlain(saved, kProjectNamespace);

    const restored = literalOf(reloaded, literal.tileId);
    assert.equal(restored.uniqueId, literal.uniqueId);
    assert.equal(restored.displayName, kFirstName);
    assert.deepEqual(imageGridBytes(restored.value), expectedBytes(kDrawnDigits));
    assert.deepEqual(paintedFrames(loading, reloaded), [expectedBytes(kDrawnDigits)]);
  });

  test("carries an edit made before the save", () => {
    const authoring = createMicroBitV2Environment();
    const { brainDef, literal } = oneImageBrain(authoring);
    new EditLiteralCommand(brainDef, literal, { value: imageValue(kEditedDigits) }).execute();
    const saved = JSON.parse(JSON.stringify(encodePersistedBrainJson(brainDef, kProjectNamespace))) as unknown;

    const loading = createMicroBitV2Environment();
    const reloaded = loading.deserializeBrainJsonFromPlain(saved, kProjectNamespace);

    assert.deepEqual(imageGridBytes(literalOf(reloaded, literal.tileId).value), expectedBytes(kEditedDigits));
    assert.deepEqual(paintedFrames(loading, reloaded), [expectedBytes(kEditedDigits)]);
  });
});

describe("a duplicate of a drawn image", () => {
  test("is a literal of its own, and an edit of the image it was drawn from never reaches it", () => {
    const environment = createMicroBitV2Environment();
    const brainDef = BrainDef.emptyBrainDef(environment.brainServices, "duplicated image brain");
    const first = drawImage(environment, brainDef, kDrawnDigits, kFirstName);
    const second = drawImage(environment, brainDef, kDrawnDigits, kSecondName);
    const zeroDuration = zeroDurationLiteral(environment, brainDef);
    const page = brainDef.pages().get(0);
    assert.ok(page, "a fresh brain document holds one page");
    const secondRule = page.appendNewRule();
    assert.ok(secondRule, "a page takes a second rule");
    appendDrawRule(environment, firstRule(brainDef), first, zeroDuration);
    appendDrawRule(environment, secondRule, second, zeroDuration);

    assert.notEqual(second.tileId, first.tileId);
    assert.deepEqual(paintedFrames(environment, brainDef), [expectedBytes(kDrawnDigits), expectedBytes(kDrawnDigits)]);

    new EditLiteralCommand(brainDef, first, { value: imageValue(kEditedDigits) }).execute();

    assert.deepEqual(paintedFrames(environment, brainDef), [expectedBytes(kEditedDigits), expectedBytes(kDrawnDigits)]);
  });
});
