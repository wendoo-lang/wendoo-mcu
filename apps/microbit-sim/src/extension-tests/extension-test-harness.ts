import assert from "node:assert/strict";
import { fileURLToPath } from "node:url";
import type { FileContent } from "@wendoo/app-host";
import { applyCatalogMove, parseExtensionReference } from "@wendoo/app-host";
import type { EmbeddedExtension, FetchedExtensionContentMap } from "@wendoo/bridge-app";
import { resolveProjectExtensions } from "@wendoo/bridge-app";
import { buildEmbeddedExtensionFromDir } from "@wendoo/bridge-app/node";
import type { CompiledActionBundle } from "@wendoo/core";
import type { WendooEnvironment } from "@wendoo/core/app";
import type { IBrainTileDef } from "@wendoo/core/brain";
import type { TypeId } from "@wendoo/core/runtime";
import { createWorkspaceCompiler, type Mount, qualifiedClassName, type WorkspaceSnapshot } from "@wendoo/ts-compiler";
import { createMicroBitV2Environment } from "@wendoo/wodal/targets/microbit-v2";
import { microbitLibraryCatalog, microbitLibraryCatalogMoves } from "../services/microbit-extension-browser";
import {
  CODAL_LIB_COORDINATE,
  CODAL_POSITION_EXT_COORDINATE,
  CORE_LIB_COORDINATE,
  MICROBIT_V2_LIB_COORDINATE,
  MICROBIT_V2_TARGET_COORDINATE,
  microbitDefaultExtensions,
} from "../services/microbit-extension-coordinates";
import { publishedLibraryFetched } from "./published-library-fixtures";

function extensionDir(relativePath: string): string {
  return fileURLToPath(new URL(relativePath, import.meta.url));
}

/**
 * Registry name the Position struct type is registered under: the Position
 * add-on's coordinate namespace, its entry module, and the `Position` binding.
 * The gamepad tiles reference it across the `@lib` boundary, but its accessor
 * and variable-factory tiles register under this declaring namespace.
 */
export const POSITION_IDENTITY = qualifiedClassName(CODAL_POSITION_EXT_COORDINATE, "/index.ts", "Position");

/** Namespace of the host workspace project the harness compiles its test-only workspace tiles into. */
export const HOST_PROJECT_NAMESPACE = "microbit-extension-tests";

/** The Cutebot chassis library's coordinate; test-only tiles that reach the arbitrator compile into this namespace. */
export { CUTEBOT_EXT_COORDINATE, YAHBOOM_GAMEPAD_EXT_COORDINATE } from "../services/microbit-extension-coordinates";

/**
 * The microbit-sim embed record: the runnable target and the three platform
 * layers, each assembled from its own `wendoo.json` `files` list through the
 * shared loader -- the single content-assembly path the app's Vite provider
 * also uses. Feature libraries are not bundled; they resolve as published `gh:`
 * content served by the fixture snapshots.
 */
function baseEmbedRecord(): EmbeddedExtension[] {
  return [
    buildEmbeddedExtensionFromDir(extensionDir("../../target-package"), MICROBIT_V2_TARGET_COORDINATE),
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

/** An extra test-only source file compiled into an extension's namespace. */
export interface ExtensionFileOverlay {
  /** Coordinate of the extension namespace the file joins. */
  coordinate: string;
  /** Extension-relative file name. */
  path: string;
  /** Source content. */
  content: string;
}

/** Inputs describing what a spec installs and which test-only tiles it compiles. */
export interface HarnessOptions {
  /** Coordinates to install on top of the seeded micro:bit v2 target. */
  install: readonly string[];
  /** Test-only tile sources compiled in the host workspace, keyed by file name. Ids are minted. */
  workspaceTiles?: Readonly<Record<string, string>>;
  /**
   * Test-only tile sources compiled into an installed extension's own namespace,
   * so they may import that extension's internal modules relatively. Read-only
   * extension source, so each declaration must carry an explicit stable id.
   */
  extensionTiles?: readonly ExtensionFileOverlay[];
}

/** A compiled extension test environment: the runtime env, the combined bundle, and tile lookups. */
export interface ExtensionTestHarness {
  /** The micro:bit v2 runtime environment with the compiled bundle installed. */
  env: WendooEnvironment;
  /** The combined action bundle spanning the workspace and every installed extension. */
  bundle: CompiledActionBundle;
  /** Resolves a compiled tile by its display label. */
  userTile(name: string): IBrainTileDef;
  /** The registered TypeId of the Position struct type (requires the Position add-on installed). */
  positionTypeId(): TypeId;
}

/** The reference an install of `coordinate` writes: the catalog entry's pinned `gh:` reference when one is listed, otherwise the coordinate's `embedded:` reference redirected through the catalog moves. */
function installReferenceFor(coordinate: string): string {
  const entry = microbitLibraryCatalog.entries.find((candidate) => candidate.coordinate === coordinate);
  if (entry !== undefined) {
    return entry.ref;
  }
  const applied = applyCatalogMove(`embedded:${coordinate}`, microbitLibraryCatalogMoves);
  assert.ok(applied.ok, `catalog move application failed for "${coordinate}"`);
  return applied.reference;
}

/** The published fixture content with the given test-only overlay files added to their libraries' snapshots, keyed by `gh:` reference. */
function fetchedWithOverlays(overlays: readonly ExtensionFileOverlay[]): FetchedExtensionContentMap {
  if (overlays.length === 0) {
    return publishedLibraryFetched;
  }
  const fetched = new Map<string, ReadonlyMap<string, FileContent>>();
  for (const [reference, files] of publishedLibraryFetched) {
    const parsed = parseExtensionReference(reference);
    const coordinate = parsed?.transport === "gh" ? `${parsed.owner}/${parsed.repo}` : undefined;
    const extra = overlays.filter((overlay) => overlay.coordinate === coordinate);
    if (extra.length === 0) {
      fetched.set(reference, files);
      continue;
    }
    const overlaid = new Map(files);
    for (const overlay of extra) {
      overlaid.set(`/${overlay.path}`, overlay.content);
    }
    fetched.set(reference, overlaid);
  }
  return fetched;
}

/**
 * Compile the installed driver libraries and any test-only tiles into one
 * micro:bit v2 environment, mirroring how the app resolves and mounts its
 * extension content. Driver tiles come from their published library
 * namespaces, served by the fixture snapshots; test-only scaffolding tiles are
 * compiled either in the host workspace or, when they must reach a library's
 * internal modules, into that library's namespace.
 */
export function buildExtensionTestHarness(options: HarnessOptions): ExtensionTestHarness {
  const overlays = options.extensionTiles ?? [];
  const embedRecord: EmbeddedExtension[] = baseEmbedRecord().map((extension) => {
    const extra = overlays.filter((overlay) => overlay.coordinate === extension.canonicalOrigin);
    if (extra.length === 0) {
      return extension;
    }
    return { ...extension, files: [...extension.files, ...extra.map((o) => ({ path: o.path, content: o.content }))] };
  });

  // The production default seed: the runnable target alone, whose manifest
  // reaches the standard-library layers through target edges.
  const extensions: Record<string, string> = { ...microbitDefaultExtensions };
  for (const coordinate of options.install) {
    // Mirror the app's install actions: a catalog offer writes its entry's
    // pinned `gh:` reference; any other coordinate writes its move-redirected
    // reference.
    extensions[coordinate] = installReferenceFor(coordinate);
  }
  const resolved = resolveProjectExtensions(extensions, {
    embedded: embedRecord,
    fetched: fetchedWithOverlays(overlays),
    moves: microbitLibraryCatalogMoves,
  });

  const env = createMicroBitV2Environment();
  const mounts: readonly Mount[] = [];
  const compiler = createWorkspaceCompiler({
    projectNamespace: HOST_PROJECT_NAMESPACE,
    mounts,
    environment: env,
    dependencies: resolved.dependencies,
    dependencyMounts: resolved.dependencyMounts,
  });

  const snapshot: WorkspaceSnapshot = new Map(
    Object.entries(options.workspaceTiles ?? {}).map(([path, content]) => [
      path,
      { kind: "file", content, etag: "e0", isReadonly: false },
    ])
  );
  compiler.replaceWorkspace(snapshot);
  const result = compiler.compile();
  assert.equal(
    result.projectResult.tsErrors.size,
    0,
    `Unexpected TypeScript diagnostics: ${JSON.stringify([...result.projectResult.tsErrors])}`
  );
  for (const root of result.rootResults) {
    assert.equal(root.tsErrors.size, 0, `Unexpected extension diagnostics: ${JSON.stringify([...root.tsErrors])}`);
  }
  const bundle = result.bundle;
  assert.ok(bundle, "expected a compiled action bundle");
  env.replaceActionBundle(bundle);

  const tilesByName = new Map<string, IBrainTileDef>();
  for (const tile of bundle.tiles) {
    if (tile.metadata?.label) {
      tilesByName.set(tile.metadata.label, tile);
    }
  }

  return {
    env,
    bundle,
    userTile(name: string): IBrainTileDef {
      const tile = tilesByName.get(name);
      assert.ok(tile, `user tile "${name}" not found; have: ${[...tilesByName.keys()].join(", ")}`);
      return tile;
    },
    positionTypeId(): TypeId {
      const typeId = env.brainServices.runtime.types.resolveByName(POSITION_IDENTITY);
      assert.ok(typeId, "expected the position type to be registered");
      return typeId;
    },
  };
}
