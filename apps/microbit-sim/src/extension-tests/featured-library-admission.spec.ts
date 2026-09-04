/**
 * Live-path coverage for which tiles may show the model their long-form
 * documentation, over the real bundle the app compiles: both chassis libraries
 * installed from their pinned catalog entries, the Position add-on pulled in as
 * their shared dependency, and the workspace project as the host root.
 */

import assert from "node:assert/strict";
import { before, describe, test } from "node:test";
import { admitsLongFormDocs, type CatalogFeaturing } from "@wendoo/assistant-bridge";
import type { CompiledActionBundle } from "@wendoo/core";
import type { IBrainTileDef } from "@wendoo/core/brain";
import { microbitFeaturedNamespaces } from "../services/microbit-extension-browser.js";
import {
  CODAL_POSITION_EXT_COORDINATE,
  CUTEBOT_EXT_COORDINATE,
  YAHBOOM_GAMEPAD_EXT_COORDINATE,
} from "../services/microbit-extension-coordinates.js";
import { buildExtensionTestHarness, HOST_PROJECT_NAMESPACE } from "./extension-test-harness.js";

/** The session an app stands with its bundled catalog featured and the workspace project as its own. */
const featured: CatalogFeaturing = {
  featured: microbitFeaturedNamespaces,
  hostNamespace: HOST_PROJECT_NAMESPACE,
};

/** The same session with nothing featured. */
const nothingFeatured: CatalogFeaturing = { featured: new Set<string>(), hostNamespace: HOST_PROJECT_NAMESPACE };

/** A test-only sensor compiled in the host workspace, so the host root owns a tile of its own. */
const HOST_TILE_SOURCE = `import { type Context, Sensor } from "wendoo";

export default Sensor({
  name: "always",
  onExecute(ctx: Context): boolean {
    return ctx.microbit.buttonA.isPressed() >= 0;
  },
});
`;

let bundle: CompiledActionBundle;

/** Every tile of the bundle owned by `namespace` alone. */
function tilesOwnedBy(namespace: string): IBrainTileDef[] {
  return bundle.tiles.filter((tile) => tile.provenance?.owners.length === 1 && tile.provenance.owners[0] === namespace);
}

/** Whether every tile of `tiles` may show its long-form documentation under `featuring`. */
function allAdmitted(tiles: readonly IBrainTileDef[], featuring: CatalogFeaturing): boolean {
  return tiles.every((tile) => admitsLongFormDocs(tile.provenance, bundle.roots, featuring));
}

describe("featuring over the bundle the app really compiles", () => {
  before(() => {
    bundle = buildExtensionTestHarness({
      install: [CUTEBOT_EXT_COORDINATE, YAHBOOM_GAMEPAD_EXT_COORDINATE],
      workspaceTiles: { "always.ts": HOST_TILE_SOURCE },
    }).bundle;
  });

  test("carries a root for each chassis library and for the Position add-on they share", () => {
    const namespaces = bundle.roots.map((root) => root.namespace);

    assert.ok(namespaces.includes(CUTEBOT_EXT_COORDINATE), namespaces.join(", "));
    assert.ok(namespaces.includes(YAHBOOM_GAMEPAD_EXT_COORDINATE), namespaces.join(", "));
    assert.ok(namespaces.includes(CODAL_POSITION_EXT_COORDINATE), namespaces.join(", "));
    assert.equal(
      microbitFeaturedNamespaces.has(CODAL_POSITION_EXT_COORDINATE),
      false,
      "the Position add-on is a dependency, not a catalog entry"
    );
  });

  test("admits the tiles of both featured chassis libraries", () => {
    const cutebot = tilesOwnedBy(CUTEBOT_EXT_COORDINATE);
    const gamepad = tilesOwnedBy(YAHBOOM_GAMEPAD_EXT_COORDINATE);

    assert.ok(cutebot.length > 0, "the Cutebot library registers tiles");
    assert.ok(gamepad.length > 0, "the gamepad library registers tiles");
    assert.equal(allAdmitted(cutebot, featured), true);
    assert.equal(allAdmitted(gamepad, featured), true);
  });

  test("admits the shared Position add-on through both featured libraries' closures", () => {
    const position = tilesOwnedBy(CODAL_POSITION_EXT_COORDINATE);
    const closures = bundle.roots
      .filter((root) => root.namespace === CUTEBOT_EXT_COORDINATE || root.namespace === YAHBOOM_GAMEPAD_EXT_COORDINATE)
      .map((root) => root.closure);

    assert.equal(closures.length, 2);
    for (const closure of closures) {
      assert.ok(closure.includes(CODAL_POSITION_EXT_COORDINATE), closure.join(", "));
    }
    assert.ok(position.length > 0, "the Position add-on registers tiles");
    assert.equal(allAdmitted(position, featured), true);
  });

  test("admits a tile two featured libraries both own, and withholds it when only one is featured", () => {
    const shared = bundle.tiles.filter((tile) => (tile.provenance?.owners.length ?? 0) > 1);
    const oneFeatured: CatalogFeaturing = {
      featured: new Set([CUTEBOT_EXT_COORDINATE]),
      hostNamespace: HOST_PROJECT_NAMESPACE,
    };

    assert.ok(shared.length > 0, "both chassis libraries declare the same direction modifiers");
    for (const tile of shared) {
      assert.deepEqual(tile.provenance?.owners, [CUTEBOT_EXT_COORDINATE, YAHBOOM_GAMEPAD_EXT_COORDINATE], tile.tileId);
      assert.equal(admitsLongFormDocs(tile.provenance, bundle.roots, featured), true, tile.tileId);
      assert.equal(admitsLongFormDocs(tile.provenance, bundle.roots, oneFeatured), false, tile.tileId);
    }
  });

  test("withholds every library's tiles once the featured set is emptied", () => {
    const libraries = [
      ...tilesOwnedBy(CUTEBOT_EXT_COORDINATE),
      ...tilesOwnedBy(YAHBOOM_GAMEPAD_EXT_COORDINATE),
      ...tilesOwnedBy(CODAL_POSITION_EXT_COORDINATE),
    ];

    for (const tile of libraries) {
      assert.equal(
        admitsLongFormDocs(tile.provenance, bundle.roots, nothingFeatured),
        false,
        `${tile.tileId} is withheld while nothing is featured`
      );
    }
  });

  test("admits the host project's own tiles whatever is featured", () => {
    const host = tilesOwnedBy(HOST_PROJECT_NAMESPACE);

    assert.ok(host.length > 0, "the workspace project registers its own tile");
    for (const tile of host) {
      assert.equal(admitsLongFormDocs(tile.provenance, bundle.roots, nothingFeatured), true, tile.tileId);
    }
  });
});
