import assert from "node:assert/strict";
import { existsSync } from "node:fs";
import { describe, test } from "node:test";
import { fileURLToPath } from "node:url";
import { mkLiteralFactoryTileId, mkLiteralTileId } from "@wendoo/core/app";
import type { IBrainTileDef } from "@wendoo/core/brain";
import { tileSentenceWord } from "@wendoo/core/brain/language-service";
import { type BrainTileFactoryDef, manufactureLiteralTile } from "@wendoo/core/brain/tiles";
import { createDefaultLocalizer } from "@wendoo/core/localization";
import { mkImageStructValue, WODAL_IMAGE_LITERAL_FACTORY_ID, WODAL_SHARED_TYPE_IDS } from "@wendoo/wodal";
import { createMicroBitV2Environment } from "@wendoo/wodal/targets/microbit-v2";
import {
  buildMicrobitBrainEditorConfig,
  createMicrobitTileVisualResolver,
  microbitDataTypeIcons,
} from "./editor-config";
import { tileVisuals } from "./tile-visuals";

/**
 * Tile kinds whose display label derives from the tile's own data (literal
 * value, variable name, accessor field name, output name, page name) rather
 * than from the tile id.
 */
const DATA_LABELED_KINDS = new Set<string>(["literal", "variable", "accessor", "output", "page"]);

/** Separates a tile id's namespace from its local name; a word carrying it came from the id. */
const kTileIdSeparator = "->";

/** Every catalog tile of the shipped microbit-v2 environment, including hidden and deprecated tiles. */
function catalogTiles(): IBrainTileDef[] {
  const env = createMicroBitV2Environment();
  const tiles: IBrainTileDef[] = [];
  for (const catalog of env.tileCatalogs()) {
    const all = catalog.getAll();
    for (let i = 0; i < all.size(); i++) {
      tiles.push(all.get(i)!);
    }
  }
  return tiles;
}

/**
 * Every catalog tile of the shipped microbit-v2 environment that the tile
 * picker can offer. Hidden and deprecated tiles are excluded, matching the
 * suggestion engine's filtering.
 */
function visibleCatalogTiles(): IBrainTileDef[] {
  return catalogTiles().filter((tileDef) => !tileDef.hidden && !tileDef.deprecated);
}

/** Absolute on-disk path of an app-served icon URL (a root-absolute `/assets/...` path). */
function iconFilePath(iconUrl: string): string {
  return fileURLToPath(new URL(`../../public${iconUrl}`, import.meta.url));
}

describe("microbit-sim tile visuals", () => {
  test("every visible catalog tile resolves the word its own metadata authors", () => {
    const localizer = createDefaultLocalizer();
    const offenders: string[] = [];
    for (const tileDef of visibleCatalogTiles()) {
      if (DATA_LABELED_KINDS.has(tileDef.kind)) {
        continue;
      }
      const authored = tileDef.metadata?.language?.form || tileDef.metadata?.label;
      if (!authored) {
        offenders.push(tileDef.tileId);
        continue;
      }
      assert.equal(tileSentenceWord(tileDef, localizer), authored, tileDef.tileId);
    }
    assert.deepEqual(offenders, [], `tiles without an authored word: ${offenders.join(", ")}`);
  });

  test("no visible catalog tile reads as a namespaced fragment of its tile id", () => {
    const localizer = createDefaultLocalizer();
    const leaking = visibleCatalogTiles()
      .filter((tileDef) => tileSentenceWord(tileDef, localizer).includes(kTileIdSeparator))
      .map((tileDef) => tileDef.tileId);
    assert.deepEqual(leaking, [], `tiles reading as a tile-id fragment: ${leaking.join(", ")}`);
  });

  test("every visible catalog tile resolves an icon instead of the missing-tile fallback", () => {
    const resolveTileVisual = createMicrobitTileVisualResolver((url) => url);
    const offenders: string[] = [];
    for (const tileDef of visibleCatalogTiles()) {
      const iconUrl = resolveTileVisual(tileDef)?.iconUrl ?? tileDef.metadata?.iconUrl;
      if (!iconUrl) {
        offenders.push(tileDef.tileId);
      }
    }
    assert.deepEqual(offenders, [], `tiles without an icon: ${offenders.join(", ")}`);
  });

  test("every tile-visuals map entry targets a shipped catalog tile and carries an icon", () => {
    const shippedTileIds = new Set(catalogTiles().map((tileDef) => tileDef.tileId));
    const staleKeys: string[] = [];
    const iconless: string[] = [];
    for (const [tileId, visual] of tileVisuals) {
      if (!shippedTileIds.has(tileId)) {
        staleKeys.push(tileId);
      }
      if (!visual.iconUrl) {
        iconless.push(tileId);
      }
    }
    assert.deepEqual(staleKeys, [], `map keys without a shipped catalog tile: ${staleKeys.join(", ")}`);
    assert.deepEqual(iconless, [], `map entries without an icon: ${iconless.join(", ")}`);
  });

  test("every mapped tile and data-type icon URL points at an svg that exists on disk", () => {
    const iconUrls = new Set<string>();
    for (const visual of tileVisuals.values()) {
      if (visual.iconUrl) {
        iconUrls.add(visual.iconUrl);
      }
    }
    const config = buildMicrobitBrainEditorConfig({
      env: createMicroBitV2Environment(),
      resolveVfsAssetUrl: (url) => url,
    });
    for (const iconUrl of config.dataTypeIcons.values()) {
      iconUrls.add(iconUrl);
    }
    const missing = [...iconUrls].filter((iconUrl) => !existsSync(iconFilePath(iconUrl)));
    assert.deepEqual(missing, [], `icon URLs without a backing file: ${missing.join(", ")}`);
  });
});

describe("a literal tile the visuals map does not name", () => {
  test("takes the icon its value type carries, not the missing-tile fallback", () => {
    const env = createMicroBitV2Environment();
    const factoryTileDef = env.brainServices.edit.tiles.get(mkLiteralFactoryTileId(WODAL_IMAGE_LITERAL_FACTORY_ID));
    assert.ok(factoryTileDef, "expected the image literal factory to be registered");
    const minted = manufactureLiteralTile(
      factoryTileDef as BrainTileFactoryDef,
      undefined,
      mkImageStructValue(5, 5, new Array(25).fill(0))
    );
    assert.ok(minted, "expected the factory to mint an image literal");
    assert.equal(tileVisuals.has(minted.tileId), false, "a minted literal has no entry of its own");

    const resolveTileVisual = createMicrobitTileVisualResolver((url) => url);

    assert.equal(resolveTileVisual(minted).iconUrl, microbitDataTypeIcons.get(WODAL_SHARED_TYPE_IDS.Image));
  });

  test("leaves a literal the map does name with the icon the map gives it", () => {
    const heartTileId = mkLiteralTileId(WODAL_SHARED_TYPE_IDS.Image, "heart");
    const heartTileDef = catalogTiles().find((tileDef) => tileDef.tileId === heartTileId);
    assert.ok(heartTileDef, heartTileId);

    const resolveTileVisual = createMicrobitTileVisualResolver((url) => url);

    assert.equal(resolveTileVisual(heartTileDef).iconUrl, tileVisuals.get(heartTileId)?.iconUrl);
    assert.notEqual(resolveTileVisual(heartTileDef).iconUrl, microbitDataTypeIcons.get(WODAL_SHARED_TYPE_IDS.Image));
  });
});
