/**
 * Live-path coverage for cross-extension user-tile registration when two
 * installed extensions share a third extension's struct type across the `@lib`
 * boundary: the Yahboom gamepad's `decoded stick position` (returnType
 * `Position`) and the Cutebot's `cutebot steer` (a `Position` param) both
 * reference the Position add-on's type. It resolves the gamepad + Cutebot
 * coordinates through the real embed record and transitive resolver, compiles
 * them, and drives the same bridge-app registration entry points the app uses --
 * `collectMetadataFromCompile` and `applyCompiledUserTiles` -- to pin that the
 * collected metadata is JSON-serializable and that applying the compiled bundle
 * registers both cross-extension tiles.
 */

import assert from "node:assert/strict";
import { before, describe, test } from "node:test";
import { fileURLToPath } from "node:url";
import { fileContentText } from "@wendoo/app-host";
import type { EmbeddedExtension } from "@wendoo/bridge-app";
import { applyCompiledUserTiles, collectMetadataFromCompile, resolveProjectExtensions } from "@wendoo/bridge-app";
import { buildEmbeddedExtensionFromDir } from "@wendoo/bridge-app/node";
import type { WendooEnvironment } from "@wendoo/core/app";
import { mkActuatorTileId, mkSensorTileId } from "@wendoo/core/app";
import type { IBrainTileDef } from "@wendoo/core/brain";
import { createWorkspaceCompiler, type WorkspaceCompileResult, type WorkspaceSnapshot } from "@wendoo/ts-compiler";
import { createMicroBitV2Environment } from "@wendoo/wodal/targets/microbit-v2";
import {
  groupTilesByLibrary,
  type TileSourceLibrary,
  tileSourceNamespace,
} from "../../../../external/wendoo-lang/packages/ui/src/brain-editor/tile-library-groups.js";
import { createMicrobitTileVisualResolver } from "../brain/editor-config.js";
import { createMicrobitDocsRegistry } from "../docs/docs-registry.js";
import { microbitLibraryCatalogMoves } from "../services/microbit-extension-browser.js";
import {
  CODAL_LIB_COORDINATE,
  CODAL_POSITION_EXT_COORDINATE,
  CORE_LIB_COORDINATE,
  CUTEBOT_EXT_COORDINATE,
  MICROBIT_V2_LIB_COORDINATE,
  MICROBIT_V2_LIB_REFERENCE,
  MICROBIT_V2_TARGET_COORDINATE,
  YAHBOOM_GAMEPAD_EXT_COORDINATE,
} from "../services/microbit-extension-coordinates.js";
import { CUTEBOT_GH_REF, publishedLibraryFetched, YAHBOOM_GAMEPAD_GH_REF } from "./published-library-fixtures.js";

const DECODED_LABEL = "decoded stick position";
const STEER_LABEL = "cutebot steer";

function extensionDir(relativePath: string): string {
  return fileURLToPath(new URL(relativePath, import.meta.url));
}

/** The microbit-sim embed record: the runnable target and the three platform layers; the feature libraries and Position resolve as fixture-served published gh: content. */
function embedRecord(): EmbeddedExtension[] {
  return [
    buildEmbeddedExtensionFromDir(extensionDir("../.."), MICROBIT_V2_TARGET_COORDINATE, "target"),
    buildEmbeddedExtensionFromDir(
      extensionDir("../../../../packages/wodal/targets/microbit-v2/lib"),
      MICROBIT_V2_LIB_COORDINATE
    ),
    buildEmbeddedExtensionFromDir(extensionDir("../../../../packages/wodal/lib"), CODAL_LIB_COORDINATE),
    buildEmbeddedExtensionFromDir(
      extensionDir("../../../../external/wendoo-lang/packages/core/lib"),
      CORE_LIB_COORDINATE
    ),
  ];
}

/** The project extensions map both libraries install through, alongside the seeded platform layer. */
const projectExtensions: Record<string, string> = {
  [MICROBIT_V2_LIB_COORDINATE]: MICROBIT_V2_LIB_REFERENCE,
  [YAHBOOM_GAMEPAD_EXT_COORDINATE]: YAHBOOM_GAMEPAD_GH_REF,
  [CUTEBOT_EXT_COORDINATE]: CUTEBOT_GH_REF,
};

/** Compile the gamepad + Cutebot libraries into a fresh micro:bit v2 environment via the real transitive resolver. */
function compileGamepadAndCutebot(env: WendooEnvironment): WorkspaceCompileResult {
  const resolved = resolveProjectExtensions(projectExtensions, {
    embedded: embedRecord(),
    fetched: publishedLibraryFetched,
    moves: microbitLibraryCatalogMoves,
  });
  const compiler = createWorkspaceCompiler({
    projectNamespace: "cross-extension-metadata",
    mounts: [],
    environment: env,
    dependencies: resolved.dependencies,
    dependencyMounts: resolved.dependencyMounts,
  });
  compiler.replaceWorkspace(new Map() as WorkspaceSnapshot);
  const result = compiler.compile();
  assert.equal(
    result.projectResult.tsErrors.size,
    0,
    `project TS errors: ${JSON.stringify([...result.projectResult.tsErrors])}`
  );
  for (const root of result.rootResults) {
    assert.equal(root.tsErrors.size, 0, `extension TS errors: ${JSON.stringify([...root.tsErrors])}`);
  }
  return result;
}

/** Whether a tile with the given display label is registered in the environment. */
function envHasTile(env: WendooEnvironment, label: string): boolean {
  for (const catalog of env.tileCatalogs()) {
    for (const tile of catalog.getAll().toArray()) {
      if (tile.metadata?.label === label) {
        return true;
      }
    }
  }
  return false;
}

describe("cross-extension user-tile metadata", () => {
  let env: WendooEnvironment;
  let result: WorkspaceCompileResult;

  before(() => {
    env = createMicroBitV2Environment();
    result = compileGamepadAndCutebot(env);
  });

  test("the collected metadata for Position-typed cross-extension tiles is JSON-serializable", () => {
    const metadata = collectMetadataFromCompile(result);
    assert.ok(
      metadata.some((m) => m.name === DECODED_LABEL),
      "the gamepad's decoded stick position tile is collected"
    );
    assert.ok(
      metadata.some((m) => m.name === STEER_LABEL),
      "the Cutebot's steer tile is collected"
    );
    assert.doesNotThrow(() => JSON.stringify(metadata), "the metadata carries no live AST node");
  });

  test("applying the compiled bundle registers both cross-extension tiles", () => {
    const applyResult = applyCompiledUserTiles(env, result);
    assert.ok(applyResult, "applying produced a result");
    assert.ok(envHasTile(env, DECODED_LABEL), "decoded stick position registered in the environment");
    assert.ok(envHasTile(env, STEER_LABEL), "cutebot steer registered in the environment");
  });

  test("every library action tile carries a /vfs/ icon and it reaches the registered tile and the resolver", () => {
    const metadata = collectMetadataFromCompile(result);
    assert.ok(metadata.length > 0, "the compile produced tile metadata");
    const withoutIcon = metadata.filter((m) => !/^\/vfs\/.+\.svg$/.test(m.iconUrl ?? "")).map((m) => m.key);
    assert.deepEqual(withoutIcon, [], "library action tiles without a /vfs/ svg icon");

    // Kinds with no icon attach point in the library authoring surface
    // (modifier icons are raw strings without VFS resolution; accessor,
    // variable-factory, and parameter tiles are compiler-derived without
    // metadata config) stay on the app's missing-tile fallback.
    const iconlessKinds = new Set(["modifier", "accessor", "factory", "parameter"]);
    const bundle = result.bundle;
    assert.ok(bundle, "the compile produced an action bundle");
    for (const tile of bundle.tiles) {
      if (tile.kind === "sensor" || tile.kind === "actuator") {
        continue;
      }
      assert.ok(iconlessKinds.has(tile.kind), `unexpected library tile kind without an icon contract: ${tile.kind}`);
    }

    applyCompiledUserTiles(env, result);
    const resolveTileVisual = createMicrobitTileVisualResolver((url) => `resolved:${url}`);
    for (const entry of metadata) {
      const tileId = entry.kind === "sensor" ? mkSensorTileId(entry.key) : mkActuatorTileId(entry.key);
      let tileDef: IBrainTileDef | undefined;
      for (const catalog of env.tileCatalogs()) {
        tileDef ??= catalog.get(tileId);
      }
      assert.ok(tileDef, `registered tile for ${tileId}`);
      assert.equal(tileDef.metadata?.iconUrl, entry.iconUrl, `registered icon URL for ${tileId}`);
      assert.equal(resolveTileVisual(tileDef)?.iconUrl, `resolved:${entry.iconUrl}`, `resolved icon URL for ${tileId}`);
    }
  });

  test("every library tile carries docsMarkdown and it reaches the docs registry", () => {
    const metadata = collectMetadataFromCompile(result);
    assert.ok(metadata.length > 0, "the compile produced tile metadata");
    const withoutDocs = metadata.filter((m) => (m.docsMarkdown ?? "").trim() === "").map((m) => m.key);
    assert.deepEqual(withoutDocs, [], "library tiles without docsMarkdown");

    const registry = createMicrobitDocsRegistry(env, metadata);
    for (const entry of metadata) {
      const tileId = entry.kind === "sensor" ? mkSensorTileId(entry.key) : mkActuatorTileId(entry.key);
      assert.equal(registry.tiles.get(tileId)?.content, entry.docsMarkdown, `registry content for ${tileId}`);
    }
  });

  test("every library tile's identity namespace is its extension coordinate, on the metadata and the registered def", () => {
    applyCompiledUserTiles(env, result);
    const metadata = collectMetadataFromCompile(result);
    assert.ok(metadata.length > 0, "the compile produced tile metadata");
    const addOnCoordinates = new Set([
      YAHBOOM_GAMEPAD_EXT_COORDINATE,
      CUTEBOT_EXT_COORDINATE,
      CODAL_POSITION_EXT_COORDINATE,
    ]);
    for (const entry of metadata) {
      assert.ok(
        addOnCoordinates.has(entry.namespace),
        `metadata namespace is a library coordinate: ${entry.key} -> ${entry.namespace}`
      );
      const tileId = entry.kind === "sensor" ? mkSensorTileId(entry.key) : mkActuatorTileId(entry.key);
      let tileDef: IBrainTileDef | undefined;
      for (const catalog of env.tileCatalogs()) {
        tileDef ??= catalog.get(tileId);
      }
      assert.ok(tileDef, `registered tile for ${tileId}`);
      assert.equal(tileSourceNamespace(tileDef), entry.namespace, `registered identity namespace for ${tileId}`);
    }
  });

  test("the resolved closure names each library from its manifest and grouping clusters registered tiles by coordinate", () => {
    applyCompiledUserTiles(env, result);
    const resolved = resolveProjectExtensions(projectExtensions, {
      embedded: embedRecord(),
      fetched: publishedLibraryFetched,
      moves: microbitLibraryCatalogMoves,
    });

    const cutebotContent = publishedLibraryFetched.get(CUTEBOT_GH_REF);
    assert.ok(cutebotContent, "the Cutebot published snapshot is served by the fixture");
    const cutebotManifestFile = cutebotContent.get("/wendoo.json");
    assert.ok(cutebotManifestFile, "the Cutebot snapshot carries a wendoo.json");
    const cutebotManifest = JSON.parse(fileContentText(cutebotManifestFile) ?? "") as { name: string };
    const cutebotOrigin = resolved.origins.find((origin) => origin.origin === CUTEBOT_EXT_COORDINATE);
    assert.equal(cutebotOrigin?.name, cutebotManifest.name, "the origin display name comes from the library manifest");

    const libraries: TileSourceLibrary[] = resolved.origins.map((origin) => ({
      coordinate: origin.origin,
      name: origin.name,
    }));
    const allTiles: IBrainTileDef[] = [];
    for (const catalog of env.tileCatalogs()) {
      allTiles.push(...catalog.getAll().toArray());
    }
    const groups = groupTilesByLibrary(allTiles, (tileDef) => tileDef, libraries);

    const clusterCoordinates = groups.clusters.map((cluster) => cluster.library.coordinate);
    assert.ok(clusterCoordinates.includes(CUTEBOT_EXT_COORDINATE), "a Cutebot cluster forms");
    assert.ok(clusterCoordinates.includes(YAHBOOM_GAMEPAD_EXT_COORDINATE), "a gamepad cluster forms");
    for (const cluster of groups.clusters) {
      for (const tileDef of cluster.items) {
        assert.equal(
          tileSourceNamespace(tileDef),
          cluster.library.coordinate,
          `cluster member namespace for ${tileDef.tileId}`
        );
      }
    }
    for (const tileDef of groups.unattributed) {
      const namespace = tileSourceNamespace(tileDef);
      assert.ok(
        namespace === undefined || !clusterCoordinates.includes(namespace),
        `unattributed tile carries no clustered library namespace: ${tileDef.tileId}`
      );
    }
  });
});
