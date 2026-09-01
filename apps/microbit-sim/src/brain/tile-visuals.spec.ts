import assert from "node:assert/strict";
import { existsSync } from "node:fs";
import { describe, test } from "node:test";
import { fileURLToPath } from "node:url";
import type { IBrainTileDef } from "@wendoo/core/brain";
import { tileSentenceWord } from "@wendoo/core/brain/language-service";
import { createDefaultLocalizer } from "@wendoo/core/localization";
import { createMicroBitV2Environment } from "@wendoo/wodal/targets/microbit-v2";
import { buildMicrobitBrainEditorConfig, createMicrobitTileVisualResolver } from "./editor-config";
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
