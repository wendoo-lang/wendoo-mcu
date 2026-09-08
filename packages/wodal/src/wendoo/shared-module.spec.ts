import assert from "node:assert/strict";
import { describe, test } from "node:test";
import { extractNumberValue, mkLiteralFactoryTileId, type Value, type WendooEnvironment } from "@wendoo/core/app";
import { type IBrainTileDef, type ITileCatalog, mkUniqueLiteralTileId } from "@wendoo/core/brain";
import { BrainDef } from "@wendoo/core/brain/model";
import { type BrainTileFactoryDef, type BrainTileLiteralDef, manufactureLiteralTile } from "@wendoo/core/brain/tiles";
import { bufferToHex, isBufferValue, isStructValue } from "@wendoo/core/runtime";
import { createWodalEnvironment } from "./environment";
import { mkImageStructValue } from "./image-value";
import { WODAL_IMAGE_LITERAL_FACTORY_ID } from "./shared-module";
import { ImageField, WODAL_SHARED_TYPE_IDS } from "./shared-type-ids";

/** Tile id of the registered `Image` literal factory. */
const IMAGE_FACTORY_TILE_ID = mkLiteralFactoryTileId(WODAL_IMAGE_LITERAL_FACTORY_ID);

/** A wodal environment installing the shared module. */
function createEnvironment(): WendooEnvironment {
  return createWodalEnvironment("microbit-v2");
}

/** The registered `Image` literal factory tile of `env`. */
function imageFactory(env: WendooEnvironment): BrainTileFactoryDef {
  const tileDef = env.brainServices.edit.tiles.get(IMAGE_FACTORY_TILE_ID);
  assert.ok(tileDef, IMAGE_FACTORY_TILE_ID);
  return tileDef as BrainTileFactoryDef;
}

/** Ids of the literal tiles `catalog` holds. */
function mintedTileIds(catalog: ITileCatalog): string[] {
  const ids: string[] = [];
  catalog.getAll().forEach((tileDef: IBrainTileDef) => {
    if (tileDef.kind === "literal") ids.push(tileDef.tileId);
  });
  return ids;
}

/** Brightness bytes of a 5x5 image whose first cell is `first`. */
function pixels(first: number): number[] {
  const bytes = new Array<number>(25).fill(0);
  bytes[0] = first;
  return bytes;
}

describe("the Image literal factory tile", () => {
  test("registers under its factory tile id and produces the shared Image type", () => {
    const factoryTileDef = imageFactory(createEnvironment());

    assert.equal(factoryTileDef.kind, "factory");
    assert.equal(factoryTileDef.factoryId, WODAL_IMAGE_LITERAL_FACTORY_ID);
    assert.equal(factoryTileDef.producedDataType, WODAL_SHARED_TYPE_IDS.Image);
    assert.equal(factoryTileDef.tileId, IMAGE_FACTORY_TILE_ID);
  });

  test("mints a persisted literal whose id is derived from its own unique identity", () => {
    const env = createEnvironment();

    const minted = manufactureLiteralTile(imageFactory(env), undefined, mkImageStructValue(5, 5, pixels(255))) as
      | BrainTileLiteralDef
      | undefined;

    assert.ok(minted);
    assert.equal(minted.valueType, WODAL_SHARED_TYPE_IDS.Image);
    assert.ok(minted.uniqueId);
    assert.equal(minted.tileId, mkUniqueLiteralTileId(minted.uniqueId));
    assert.equal(minted.persist, true);
  });

  test("mints a value label carrying neither a tile-id separator nor a dot", () => {
    const env = createEnvironment();

    const minted = manufactureLiteralTile(imageFactory(env), undefined, mkImageStructValue(5, 5, pixels(255))) as
      | BrainTileLiteralDef
      | undefined;

    assert.ok(minted);
    assert.equal(minted.valueLabel.includes("->"), false);
    assert.equal(minted.valueLabel.includes("."), false);
  });

  test("mints a catalog entry per manufacture of the same image content", () => {
    const env = createEnvironment();
    const catalog = BrainDef.emptyBrainDef(env.brainServices, "image literal mint").catalog();

    const first = manufactureLiteralTile(imageFactory(env), catalog, mkImageStructValue(5, 5, pixels(255)));
    const again = manufactureLiteralTile(imageFactory(env), catalog, mkImageStructValue(5, 5, pixels(255)));

    assert.ok(first && again);
    assert.notEqual(again.tileId, first.tileId);
    assert.deepEqual(mintedTileIds(catalog).sort(), [again.tileId, first.tileId].sort());
  });

  test("mints a separate catalog entry per distinct image content", () => {
    const env = createEnvironment();
    const catalog = BrainDef.emptyBrainDef(env.brainServices, "image literal mint variants").catalog();

    const lit = manufactureLiteralTile(imageFactory(env), catalog, mkImageStructValue(5, 5, pixels(255)));
    const dim = manufactureLiteralTile(imageFactory(env), catalog, mkImageStructValue(5, 5, pixels(17)));

    assert.ok(lit && dim);
    assert.notEqual(lit.tileId, dim.tileId);
    assert.deepEqual(mintedTileIds(catalog).sort(), [dim.tileId, lit.tileId].sort());
  });

  test("names the minted literal with the submitted display name", () => {
    const env = createEnvironment();
    const catalog = BrainDef.emptyBrainDef(env.brainServices, "image literal naming").catalog();

    const minted = manufactureLiteralTile(
      imageFactory(env),
      catalog,
      mkImageStructValue(5, 5, pixels(255)),
      undefined,
      "rock"
    );

    assert.ok(minted);
    assert.equal(minted.displayName, "rock");
    assert.equal(catalog.get(minted.tileId), minted);
  });

  test("mints a literal whose struct slots carry the dimensions and byte-exact pixels", () => {
    const env = createEnvironment();
    const bytes = pixels(136);

    const minted = manufactureLiteralTile(imageFactory(env), undefined, mkImageStructValue(5, 5, bytes)) as
      | BrainTileLiteralDef
      | undefined;

    assert.ok(minted);
    const value = minted.value as Value | undefined;
    assert.ok(isStructValue(value));
    assert.equal(value.typeId, WODAL_SHARED_TYPE_IDS.Image);
    assert.equal(extractNumberValue(value.v?.at(ImageField.Width)), 5);
    assert.equal(extractNumberValue(value.v?.at(ImageField.Height)), 5);
    const buffer = value.v?.at(ImageField.Pixels);
    assert.ok(isBufferValue(buffer));
    assert.equal(bufferToHex(buffer), bytes.map((byte) => byte.toString(16).padStart(2, "0")).join(""));
  });

  test("refuses to mint without a value to carry", () => {
    const factoryTileDef = imageFactory(createEnvironment());

    assert.equal(factoryTileDef.manufacture(factoryTileDef, {}), undefined);
  });

  test("refuses to mint a value that is not an Image struct", () => {
    const factoryTileDef = imageFactory(createEnvironment());

    assert.equal(factoryTileDef.manufacture(factoryTileDef, { value: "0123456789abcdef012345678" }), undefined);
    assert.equal(factoryTileDef.manufacture(factoryTileDef, { value: 42 }), undefined);
    assert.equal(factoryTileDef.manufacture(factoryTileDef, { value: true }), undefined);
  });
});
