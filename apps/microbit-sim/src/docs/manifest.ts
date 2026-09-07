// ---------------------------------------------------------------------------
// App-specific documentation curation. Host tile docs entries are derived
// from the environment's tile catalog at registry-build time; these maps hold
// the hand-curated exceptions, the pattern page metadata, and the markdown
// content, keyed by tile id or content key. Pattern and concept bodies live as
// .md files under content/en/patterns/ and content/en/concepts/; host tile
// bodies live with their tiles, in the wodal package's microbit-v2 docs
// directory. All of them reach this module through the generated locale module
// (npm run generate:docs).
// ---------------------------------------------------------------------------

import type { BrainTileKind } from "@wendoo/core/app";
import type { AppPatternDocMeta } from "@wendoo/docs";
import { MICROBIT_V2_TILE_DOCS } from "@wendoo/wodal/targets/microbit-v2";
import { tileContent } from "./_generated/en";

/**
 * Docs sidebar category for each tile kind that gets a derived docs entry.
 * Visible catalog tiles of kinds absent here get no entry.
 */
export const tileKindCategories: Partial<Record<BrainTileKind, string>> = {
  sensor: "Sensors",
  actuator: "Actuators",
  modifier: "Parameters & Modifiers",
  parameter: "Parameters & Modifiers",
  literal: "Literals",
};

/**
 * Visible catalog tiles deliberately left without a docs entry. The boolean
 * and nil value literals are covered by the core Literals concept page.
 */
export const undocumentedTileIds: ReadonlySet<string> = new Set([
  "tile.literal->boolean:<boolean>->true",
  "tile.literal->boolean:<boolean>->false",
  "tile.literal->nil:<nil>->nil",
]);

/**
 * Docs sidebar category overrides, keyed by tile id, for tiles whose
 * kind-derived category is wrong, and for tiles whose kind gets no category of
 * its own.
 */
export const tileCategoryOverrides: Readonly<Record<string, string>> = {
  "tile.lit.factory->image": "Literals",
};

/**
 * Pattern doc pages. Each entry's `contentKey` is a filename stem under
 * content/en/patterns/.
 */
export const patternDocs: readonly AppPatternDocMeta[] = [
  {
    id: "button-press-response",
    title: "Respond to a Button Press",
    tags: ["buttons", "display", "basics"],
    category: "Basics",
    contentKey: "button-press-response",
  },
];

/**
 * Title for each concept doc page, keyed by id (a filename stem under
 * content/en/concepts/).
 */
export const conceptTitles: Readonly<Record<string, string>> = {
  vscode: "Connect VS Code",
  about: "About this App",
};

/** Search tags for each concept doc page, keyed by id. */
export const conceptTags: Readonly<Record<string, string[]>> = {
  vscode: ["vscode", "bridge", "typescript"],
  about: [],
};

/** Sidebar display order for concept doc pages, by id. */
export const conceptOrder: readonly string[] = ["vscode", "about"];

/**
 * Markdown documentation bodies keyed by tile id. Tiles without a body render
 * the sidebar's no-documentation fallback.
 */
export const tileDocContent: Readonly<Record<string, string>> = Object.fromEntries(
  MICROBIT_V2_TILE_DOCS.map(({ tileId, contentKey }) => [tileId, tileContent[contentKey] ?? ""])
);
