/**
 * Live-path coverage for which tiles may show the model their long-form
 * documentation, over the real bundle the app compiles: both chassis libraries
 * installed from their pinned catalog entries, the Position add-on pulled in as
 * their shared dependency, and the workspace project as the host root.
 */

import assert from "node:assert/strict";
import { before, describe, test } from "node:test";
import type { CatalogTile } from "@wendoo/assistant-bridge";
import { admitsLongFormDocs, type CatalogFeaturing, catalogTiles, readCatalog } from "@wendoo/assistant-bridge";
import { createEditedBrainWorkspaces } from "@wendoo/assistant-panel";
import type { CompiledActionBundle } from "@wendoo/core";
import type { IBrainTileDef } from "@wendoo/core/brain";
import { BrainCommandHistory, BrainDef } from "@wendoo/core/brain/model";
import { createTargetAdapter } from "@wendoo/wodal/targets/microbit-v2/rehearsal";
import { microbitFeaturedNamespaces } from "../services/microbit-extension-browser.js";
import {
  CODAL_POSITION_EXT_COORDINATE,
  CUTEBOT_EXT_COORDINATE,
  MICROBIT_V2_TARGET_COORDINATE,
  YAHBOOM_GAMEPAD_EXT_COORDINATE,
} from "../services/microbit-extension-coordinates.js";
import type { ExtensionFileOverlay, ExtensionTestHarness } from "./extension-test-harness.js";
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

/** Display name of the test-only Cutebot tile whose documentation carries an assistant section. */
const TAUGHT_TILE_NAME = "cutebot marker";

/** The description that tile's documentation opens with. */
const TAUGHT_DESCRIPTION = "Marks the run from inside the chassis library.";

/** The teaching that tile's documentation reserves for the model. */
const TAUGHT_SECTION = "Place it once a rule; it never takes the movement lease.";

/**
 * A test-only actuator compiled into the Cutebot library's own namespace, so a
 * library-owned tile carries documentation with an assistant section.
 */
const TAUGHT_TILE_SOURCE = `import { Actuator, type Context } from "wendoo";

export default Actuator({
  name: ${JSON.stringify(TAUGHT_TILE_NAME)},
  id: "wQ2rTn6bKxMv0aLd",
  docs: "./taught-marker.md",
  onExecute(ctx: Context): void {
    ctx.microbit.buttonA.isPressed();
  },
});
`;

/** The documentation that tile ships: an opening description, then its assistant section. */
const TAUGHT_TILE_DOC = `# Cutebot marker

${TAUGHT_DESCRIPTION}

\`\`\`assistant
${TAUGHT_SECTION}
\`\`\`
`;

/** The test-only tile and its documentation, overlaid on the Cutebot library's published snapshot. */
function taughtTileOverlay(): readonly ExtensionFileOverlay[] {
  const overlay = (path: string, content: string): ExtensionFileOverlay => ({
    coordinate: CUTEBOT_EXT_COORDINATE,
    path,
    content,
  });
  return [overlay("taught-marker.ts", TAUGHT_TILE_SOURCE), overlay("taught-marker.md", TAUGHT_TILE_DOC)];
}

let bundle: CompiledActionBundle;
let harness: ExtensionTestHarness;

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
    harness = buildExtensionTestHarness({
      install: [CUTEBOT_EXT_COORDINATE, YAHBOOM_GAMEPAD_EXT_COORDINATE],
      workspaceTiles: { "always.ts": HOST_TILE_SOURCE },
      extensionTiles: taughtTileOverlay(),
    });
    bundle = harness.bundle;
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

describe("the catalog line the app serves for a library tile over the bundle it compiles", () => {
  /** What the workspaces report when asked, reassigned per test. */
  let standing: CatalogFeaturing = featured;

  /** The line `read_catalog` serves for the taught Cutebot tile, through the app's own workspace seam. */
  function servedTaughtTile(): CatalogTile {
    const adapter = createTargetAdapter(MICROBIT_V2_TARGET_COORDINATE);
    const workspaces = createEditedBrainWorkspaces({
      environment: harness.env,
      adapter,
      featuring: () => standing,
    });
    const brainDef = BrainDef.emptyBrainDef(harness.env.brainServices, "admission brain");
    workspaces.setEditedBrain({
      brainDef,
      history: new BrainCommandHistory(),
      reveal: () => {},
      takeKeyboard: () => true,
    });
    const workspace = workspaces.workspaceFor(brainDef.id());
    const taught = harness.userTile(TAUGHT_TILE_NAME);
    const listed = catalogTiles(readCatalog(workspace, {})).find((tile) => tile.tileId === taught.tileId);
    assert.ok(listed, `the catalog lists ${taught.tileId}`);
    return listed;
  }

  test("carries the tile's assistant section while its library is featured", () => {
    standing = featured;

    const listed = servedTaughtTile();

    assert.deepEqual(harness.userTile(TAUGHT_TILE_NAME).provenance?.owners, [CUTEBOT_EXT_COORDINATE]);
    assert.equal(listed.description, TAUGHT_DESCRIPTION);
    assert.equal(listed.assistant, TAUGHT_SECTION);
  });

  test("carries the description but no assistant section once the featured set is emptied", () => {
    standing = nothingFeatured;

    const listed = servedTaughtTile();

    assert.equal(listed.description, TAUGHT_DESCRIPTION);
    assert.equal(listed.assistant, undefined);
  });
});
