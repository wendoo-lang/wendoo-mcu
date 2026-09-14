import "fake-indexeddb/auto";
import assert from "node:assert/strict";
import { describe, test } from "node:test";
import { fileURLToPath } from "node:url";
import {
  buildActiveProjectExportDocument,
  createIdbProjectStore,
  DEFAULT_PROJECT_NAME,
  importProjectDocument,
  ProjectManager,
} from "@wendoo/app-host";
import type { EmbeddedExtension } from "@wendoo/bridge-app";
import { AppEnvironmentHost } from "@wendoo/bridge-app";
import { buildEmbeddedExtensionFromDir } from "@wendoo/bridge-app/node";
import { coreModule } from "@wendoo/core/app";
import { createProfileNumerics } from "@wendoo/core/runtime";
import { isCompilerControlledPath } from "@wendoo/ts-compiler";
import { createWodalSharedModule, getWodalDeviceProfile, WodalDeviceProfileId } from "@wendoo/wodal";
import { microbitLibraryCatalogMoves } from "../services/microbit-extension-browser";
import {
  CODAL_LIB_COORDINATE,
  CORE_LIB_COORDINATE,
  MICROBIT_V2_LIB_COORDINATE,
  MICROBIT_V2_TARGET_COORDINATE,
  MICROBIT_V2_TARGET_REFERENCE,
  microbitDefaultExtensions,
} from "../services/microbit-extension-coordinates";
import { MICROBIT_SIM_APP_CHUNK_KEY, translateMicrobitSimAppChunk } from "../services/project-io";
import { createPublishedLibraryFixtureTransport } from "./published-library-fixtures";

// The app-host reads localStorage/sessionStorage; provide an in-memory shim
// alongside fake-indexeddb so the host runs headlessly.
function memoryStorage(): Storage {
  const data = new Map<string, string>();
  return {
    get length() {
      return data.size;
    },
    clear: () => data.clear(),
    getItem: (key) => data.get(key) ?? null,
    key: (index) => [...data.keys()][index] ?? null,
    removeItem: (key) => {
      data.delete(key);
    },
    setItem: (key, value) => {
      data.set(key, String(value));
    },
  } as Storage;
}
const globalShim = globalThis as unknown as { localStorage?: Storage; sessionStorage?: Storage };
globalShim.localStorage ??= memoryStorage();
globalShim.sessionStorage ??= memoryStorage();

function extensionDir(relativePath: string): string {
  return fileURLToPath(new URL(relativePath, import.meta.url));
}

/**
 * The app's full embed record: the same four coordinate/directory registrations
 * as `vite/embedded-extensions.mjs`, assembled through
 * {@link buildEmbeddedExtensionFromDir}, the single content-assembly path the
 * app's Vite provider also uses.
 */
function appEmbedRecord(): EmbeddedExtension[] {
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

let storeCounter = 0;

/**
 * The host exactly as `MicrobitSimEnvironmentStore.create()` constructs it in
 * browser mode, with the headless substitutions stated where they occur: the
 * IDB store runs over fake-indexeddb, the jsDelivr transport is replaced by
 * the published-library fetch fixture (no network in tests), and the Web Locks
 * project lock and the bridge URL are omitted (browser-only surfaces the load
 * path never consumes before `initBridge`).
 */
async function makeProductionShapedHost(): Promise<{
  host: AppEnvironmentHost;
  projectManager: ProjectManager;
}> {
  const activeProfile = getWodalDeviceProfile(WodalDeviceProfileId.MICROBIT_V2);
  const projectStore = await createIdbProjectStore(`wendoo-import-targets-${storeCounter++}`);
  const projectManager = new ProjectManager(projectStore, {
    filesystemOptions: {
      shouldExclude: (path) => isCompilerControlledPath(path, []),
    },
    defaultExtensions: microbitDefaultExtensions,
  });
  const host = new AppEnvironmentHost({
    projectManager,
    modules: [coreModule(), createWodalSharedModule(), activeProfile.createWendooModule()],
    numerics: createProfileNumerics(activeProfile.numberPrecision),
    mounts: [],
    embeddedExtensions: appEmbedRecord(),
    extensionFetchTransport: createPublishedLibraryFixtureTransport(),
    catalogMoves: microbitLibraryCatalogMoves,
    rng: { next: () => Math.random() },
  });
  return { host, projectManager };
}

/** A declared target no embed record and no registry pin can resolve. */
const MISSING_PLATFORM_COORDINATE = "wendoo-lang/lib-missing-platform";

/** The imported document's declared targets map. */
const IMPORTED_TARGETS = { [MISSING_PLATFORM_COORDINATE]: { packageVersion: "^1.0.0" } } as const;

/**
 * A shared `.wendoo` export document whose manifest declares the embedded
 * target extension and a `targets` map.
 */
function targetedDocumentText(targets?: Readonly<Record<string, { packageVersion: string }>>): string {
  return JSON.stringify({
    format: "wendoo.project/2",
    manifest: {
      name: "targeted",
      version: "0.1.0",
      description: "",
      extensions: { [MICROBIT_V2_TARGET_COORDINATE]: MICROBIT_V2_TARGET_REFERENCE },
      ...(targets === undefined ? {} : { targets }),
    },
    contents: {},
  });
}

/**
 * One production-shaped import: startup on the default project,
 * `importProjectDocument` with the app's chunk translator, then the switch to
 * the created project -- the exact sequence of
 * `MicrobitSimEnvironmentStore.importProject`.
 */
async function importTargetedProject(documentText: string): Promise<{
  host: AppEnvironmentHost;
  projectManager: ProjectManager;
}> {
  const { host, projectManager } = await makeProductionShapedHost();
  await host.initialize(DEFAULT_PROJECT_NAME);
  const file = new File([documentText], "targeted.wendoo");
  const imported = await importProjectDocument(file, MICROBIT_SIM_APP_CHUNK_KEY, projectManager, {
    appChunkCallback: translateMicrobitSimAppChunk,
  });
  assert.equal(imported.success, true, JSON.stringify(imported.diagnostics));
  await host.switchProject(imported.projectId!);
  return { host, projectManager };
}

describe(".wendoo import carries the manifest targets declaration -- production wiring", () => {
  test("the imported targets persist to the stored manifest and an unresolvable one warns on load", async () => {
    const { host, projectManager } = await importTargetedProject(targetedDocumentText(IMPORTED_TARGETS));
    try {
      // Stored form: the created project's manifest carries the declaration verbatim.
      assert.deepEqual(projectManager.activeProject?.manifest.targets, IMPORTED_TARGETS);

      // Resolution form: the load resolved against the declared targets, and
      // the coordinate nothing can deliver surfaced on the warning snapshot
      // the UI consumes.
      assert.ok(
        host
          .getResolutionWarningsSnapshot()
          .some((warning) => warning.kind === "unresolved-target" && warning.origin === MISSING_PLATFORM_COORDINATE),
        `the load surfaces the unresolved target: ${JSON.stringify(host.getResolutionWarningsSnapshot())}`
      );
    } finally {
      host.dispose();
    }
  });

  test("the imported targets round-trip into an export document deep-equal to the original", async () => {
    const { host, projectManager } = await importTargetedProject(targetedDocumentText(IMPORTED_TARGETS));
    try {
      const exported = await buildActiveProjectExportDocument(projectManager);
      assert.deepEqual(exported.manifest.targets, IMPORTED_TARGETS);
    } finally {
      host.dispose();
    }
  });

  test("a document without targets imports and re-exports with the field absent", async () => {
    const { host, projectManager } = await importTargetedProject(targetedDocumentText());
    try {
      assert.equal(projectManager.activeProject?.manifest.targets, undefined);
      const exported = await buildActiveProjectExportDocument(projectManager);
      assert.equal("targets" in exported.manifest, false);
    } finally {
      host.dispose();
    }
  });
});
