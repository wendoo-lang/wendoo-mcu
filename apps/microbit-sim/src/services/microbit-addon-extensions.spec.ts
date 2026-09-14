import assert from "node:assert/strict";
import { describe, test } from "node:test";
import { fileURLToPath } from "node:url";
import type { EmbeddedExtension } from "@wendoo/bridge-app";
import {
  collectMetadataFromCompile,
  findEmbeddedExtensionsMissingStableIds,
  formatEmbeddedExtensionIdViolations,
  resolveProjectExtensions,
} from "@wendoo/bridge-app";
import { buildEmbeddedExtensionFromDir } from "@wendoo/bridge-app/node";
import { createWorkspaceCompiler, type Mount, type WorkspaceSnapshot } from "@wendoo/ts-compiler";
import { createMicroBitV2Environment } from "@wendoo/wodal/targets/microbit-v2";
import {
  CODAL_POSITION_GH_REF,
  publishedLibraryFetched,
  YAHBOOM_GAMEPAD_GH_REF,
} from "../extension-tests/published-library-fixtures";
import {
  buildMicrobitExtensionEntries,
  MICROBIT_LAYER_COORDINATES,
  microbitLibraryCatalogMoves,
} from "./microbit-extension-browser";
import {
  CODAL_LIB_COORDINATE,
  CODAL_POSITION_EXT_COORDINATE,
  CORE_LIB_COORDINATE,
  CUTEBOT_EXT_COORDINATE,
  MICROBIT_V2_LIB_COORDINATE,
  MICROBIT_V2_LIB_REFERENCE,
  MICROBIT_V2_TARGET_COORDINATE,
  YAHBOOM_GAMEPAD_EXT_COORDINATE,
} from "./microbit-extension-coordinates";

function extensionDir(relativePath: string): string {
  return fileURLToPath(new URL(relativePath, import.meta.url));
}

/**
 * The runnable target and the three microbit layer libraries assembled from
 * each extension's own `wendoo.json` `files` list through the shared
 * loader. The layer stack is core <- wodal <- microbit-v2; the target resolves
 * the published libraries' `trg-` compatibility-target edge.
 */
function microbitEmbedRecord(): EmbeddedExtension[] {
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

/** The content sources the microbit resolver reads: the embed record plus the fixture-served published gh: content and the catalog moves. */
function microbitSources() {
  return {
    embedded: microbitEmbedRecord(),
    fetched: publishedLibraryFetched,
    moves: microbitLibraryCatalogMoves,
  };
}

const microbitProject = { [MICROBIT_V2_LIB_COORDINATE]: MICROBIT_V2_LIB_REFERENCE };

describe("microbit published libraries -- every declaration ships a stable id", () => {
  test("no published library snapshot declares a Sensor, Actuator, or Conversion without an explicit stable id", () => {
    // The published snapshots assembled as embed-record-shaped bundles, so the
    // shared id gate walks their declarations.
    const published = [
      buildEmbeddedExtensionFromDir(extensionDir("../../test-fixtures/lib-elecfreaks-cutebot"), CUTEBOT_EXT_COORDINATE),
      buildEmbeddedExtensionFromDir(
        extensionDir("../../test-fixtures/lib-yahboom-gamepad"),
        YAHBOOM_GAMEPAD_EXT_COORDINATE
      ),
    ];
    const services = createMicroBitV2Environment().brainServices;
    const violations = findEmbeddedExtensionsMissingStableIds(published, services);
    assert.deepEqual(violations, [], formatEmbeddedExtensionIdViolations(violations));
  });
});

describe("microbit published libraries -- the gamepad depends on the Position library across layers", () => {
  test("installing the gamepad pulls the Position library into the resolved closure and its tiles reference Position", () => {
    const extensions: Record<string, string> = {
      [MICROBIT_V2_LIB_COORDINATE]: MICROBIT_V2_LIB_REFERENCE,
      [YAHBOOM_GAMEPAD_EXT_COORDINATE]: YAHBOOM_GAMEPAD_GH_REF,
    };
    const resolved = resolveProjectExtensions(extensions, microbitSources());

    // The gamepad's manifest edge to the Position library resolves
    // transitively through its published version-form pin, even though
    // Position targets the lower wodal layer and the gamepad targets the
    // micro:bit v2 platform. The gamepad's own compatibility-target edge to
    // the runnable target joins its dependencies alongside that extension edge.
    const origins = resolved.dependencyMounts.map((m) => m.namespace);
    assert.ok(origins.includes(YAHBOOM_GAMEPAD_EXT_COORDINATE), "the gamepad is in the closure");
    assert.ok(
      origins.includes(CODAL_POSITION_EXT_COORDINATE),
      "the Position library is pulled in as the gamepad's dep"
    );
    const gamepadMount = resolved.dependencyMounts.find((m) => m.namespace === YAHBOOM_GAMEPAD_EXT_COORDINATE)!;
    assert.deepEqual(gamepadMount.dependencies, [
      { coordinate: CODAL_POSITION_EXT_COORDINATE },
      { coordinate: MICROBIT_V2_TARGET_COORDINATE },
    ]);

    const environment = createMicroBitV2Environment();
    const mounts: readonly Mount[] = [];
    const compiler = createWorkspaceCompiler({
      projectNamespace: "microbit-gamepad-probe",
      mounts,
      environment,
      dependencies: resolved.dependencies,
      dependencyMounts: resolved.dependencyMounts,
    });
    compiler.replaceWorkspace(new Map());
    const result = compiler.compile();

    for (const root of result.rootResults) {
      assert.equal(root.tsErrors.size, 0, `gamepad closure must compile clean: ${JSON.stringify([...root.tsErrors])}`);
    }

    // The gamepad's Position-typed inline sensor lowers into the bundle, and the
    // Position struct's accessor and variable tiles register under the Position
    // library's own namespace -- proof the `@lib` struct-type reference resolves
    // across the library boundary at runtime.
    const tiles = collectMetadataFromCompile(result);
    const stickPosition = tiles.find((t) => t.name === "stick position");
    assert.ok(stickPosition, "the gamepad's Position-returning sensor is registered");
    assert.equal(stickPosition.namespace, YAHBOOM_GAMEPAD_EXT_COORDINATE);
    assert.ok(result.bundle, "a compiled bundle is produced");
    assert.ok(
      result.bundle.actions.get(stickPosition.key),
      "the Position-returning sensor's action is lowered into the bundle"
    );

    for (const rootResult of result.rootResults) {
      for (const [, compileResult] of rootResult.results) {
        for (const diag of compileResult.diagnostics) {
          assert.notEqual(diag.code, 5014, "no duplicate-stable-id diagnostic is raised across the closure");
        }
      }
    }
  });
});

describe("microbit published libraries -- browser entries list direct dependencies only", () => {
  test("a fresh microbit project does not list the Position library it does not directly reference", () => {
    const entries = buildMicrobitExtensionEntries(microbitProject, microbitEmbedRecord());

    // Position stays a transitive sub-dependency until the project directly
    // references it.
    assert.equal(
      entries.find((e) => e.coordinate === CODAL_POSITION_EXT_COORDINATE),
      undefined
    );
  });

  test("a directly-installed Position library lists as an installed gh: entry", () => {
    const project = { ...microbitProject, [CODAL_POSITION_EXT_COORDINATE]: CODAL_POSITION_GH_REF };
    const entries = buildMicrobitExtensionEntries(project, microbitEmbedRecord(), publishedLibraryFetched);

    const position = entries.find((e) => e.coordinate === CODAL_POSITION_EXT_COORDINATE);
    assert.ok(position, "a directly-referenced library is an entry card");
    assert.equal(position.installed, true);
    assert.equal(position.name, "Position");
    assert.equal(position.repoUrl, `https://github.com/${CODAL_POSITION_EXT_COORDINATE}`);
  });

  test("the locked layer coordinates cover the three platform layers only", () => {
    assert.equal(MICROBIT_LAYER_COORDINATES.has(CODAL_POSITION_EXT_COORDINATE), false);
    assert.equal(MICROBIT_LAYER_COORDINATES.has(MICROBIT_V2_LIB_COORDINATE), true);
  });
});

describe("microbit published libraries -- install materializes a usable type", () => {
  const HOST_PROGRAM = `import { Sensor, type Context } from "wendoo";
import { Position } from "@lib/wendoo-lang/lib-codal-position";

export default Sensor({
  name: "position probe",
  returnType: Position,
  inline: true,
  onExecute(ctx: Context): Position {
    return Position({ x: 0, y: 0 });
  },
});
`;

  test("installing Position lets a host program reference its published struct type across the @lib boundary", () => {
    const extensions: Record<string, string> = {
      [MICROBIT_V2_LIB_COORDINATE]: MICROBIT_V2_LIB_REFERENCE,
      [CODAL_POSITION_EXT_COORDINATE]: CODAL_POSITION_GH_REF,
    };
    const resolved = resolveProjectExtensions(extensions, microbitSources());
    const environment = createMicroBitV2Environment();
    const mounts: readonly Mount[] = [];
    const compiler = createWorkspaceCompiler({
      projectNamespace: "microbit-addon-probe",
      mounts,
      environment,
      dependencies: resolved.dependencies,
      dependencyMounts: resolved.dependencyMounts,
    });
    const snapshot: WorkspaceSnapshot = new Map([
      ["main.ts", { kind: "file", content: HOST_PROGRAM, etag: "e0", isReadonly: false }],
    ]);
    compiler.replaceWorkspace(snapshot);
    const result = compiler.compile();

    assert.equal(
      result.projectResult.tsErrors.size,
      0,
      `the host program references Position across @lib and must compile clean: ${JSON.stringify([
        ...result.projectResult.tsErrors,
      ])}`
    );

    const controlled = compiler.getCompilerControlledFiles();
    assert.ok(
      controlled.has(`.libraries/${CODAL_POSITION_EXT_COORDINATE}/index.ts`),
      "the Position extension entry materializes under .libraries/"
    );

    // The host sensor lowers, and no duplicate-stable-id diagnostic (5014) is raised.
    const tiles = collectMetadataFromCompile(result);
    assert.ok(
      tiles.some((t) => t.name === "position probe"),
      "the host sensor that returns Position is registered"
    );
    for (const rootResult of result.rootResults) {
      for (const [, compileResult] of rootResult.results) {
        for (const diag of compileResult.diagnostics) {
          assert.notEqual(diag.code, 5014, "no duplicate-stable-id diagnostic is raised");
        }
      }
    }
  });
});
