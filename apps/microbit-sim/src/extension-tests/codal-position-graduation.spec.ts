/**
 * Acceptance coverage for graduating the Position library from a host-bundled
 * embedded add-on to a published (`gh:`) library, redirected by a bundled
 * catalog transport-flip move. Proves four behaviors of the flip:
 *
 * - DIRECT: Position is a compatibility-filtered `gh:` catalog offer, and
 *   installing it resolves its published content through the fetch fixture and
 *   registers its `Position` struct type.
 * - TRANSITIVE REDIRECT: a dependency's `embedded:` reference to Position, at
 *   any depth, resolves ONLY through the move to the `gh:` reference.
 *   Red-first: with no move the dep does not resolve at all.
 * - EMBEDDED GONE: Position is absent from the embed record, and an `embedded:`
 *   reference to it no longer resolves; only the `gh:` path does.
 * - PROJECT MIGRATION: a saved project whose top-level manifest references
 *   Position through `embedded:` is rewritten to the `gh:` reference by the
 *   host's load-time move rewrite, and the migrated project still resolves it.
 *
 * The graduated chassis libraries ride the same machinery through their RENAME
 * moves: a project holding a retired embedded chassis reference is rewritten to
 * the new coordinate's `gh:` pin on load, and the fetched chassis content's own
 * version-form Position pin is healed by a fetch in the same walk.
 */

import assert from "node:assert/strict";
import { describe, test } from "node:test";
import { fileURLToPath } from "node:url";
import {
  type ActiveProject,
  createInMemoryProjectFileSystem,
  type ExtensionFetchTransport,
  type ProjectFileSystem,
  type ProjectManager,
} from "@wendoo/app-host";
import { AppEnvironmentHost, type EmbeddedExtension, resolveProjectExtensions } from "@wendoo/bridge-app";
import { buildEmbeddedExtensionFromDir } from "@wendoo/bridge-app/node";
import { coreModule } from "@wendoo/core/app";
import { createProfileNumerics } from "@wendoo/core/runtime";
import { createWodalSharedModule, getWodalDeviceProfile, WodalDeviceProfileId } from "@wendoo/wodal";
import {
  buildMicrobitCatalogOffers,
  buildMicrobitExtensionEntries,
  microbitLibraryCatalogMoves,
} from "../services/microbit-extension-browser";
import {
  CODAL_LIB_COORDINATE,
  CODAL_POSITION_EXT_COORDINATE,
  CORE_LIB_COORDINATE,
  CUTEBOT_EXT_COORDINATE,
  MICROBIT_V2_LIB_COORDINATE,
  MICROBIT_V2_LIB_REFERENCE,
  MICROBIT_V2_TARGET_COORDINATE,
  MICROBIT_V2_TARGET_REFERENCE,
  microbitDefaultExtensions,
} from "../services/microbit-extension-coordinates";
import { buildExtensionTestHarness } from "./extension-test-harness";
import {
  CODAL_POSITION_GH_REF,
  CODAL_POSITION_VERSION_REF,
  CUTEBOT_GH_REF,
  createPublishedLibraryFixtureTransport,
  publishedLibraryFetched,
} from "./published-library-fixtures";

/** The retired embedded chassis coordinate the catalog's rename move migrates away from. */
const RETIRED_CUTEBOT_COORDINATE = "wendoo-lang/lib-microbit-cutebot";

function extensionDir(relativePath: string): string {
  return fileURLToPath(new URL(relativePath, import.meta.url));
}

/**
 * The microbit-sim embed record after graduation: the target and the three
 * platform layers. Position and the chassis libraries are deliberately absent
 * -- they resolve through the catalog moves to their published gh: content.
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

/** The resolver content sources after graduation: the embed record plus the fixture-served published gh: content and the catalog moves. */
function sourcesWithMove() {
  return {
    embedded: appEmbedRecord(),
    fetched: publishedLibraryFetched,
    moves: microbitLibraryCatalogMoves,
  };
}

/** A dependency-declaring library bundle whose manifest holds the given extension edges. */
function dependentExtension(coordinate: string, extensions: Record<string, string>): EmbeddedExtension {
  return {
    canonicalOrigin: coordinate,
    files: [
      { path: "index.ts", content: "export {};" },
      {
        path: "wendoo.json",
        content: JSON.stringify({ name: "Dependent", version: "0.1.0", extensions }),
      },
    ],
  };
}

describe("Position graduation -- unlisted but resolvable", () => {
  test("Position is not a featured catalog offer or entry for a fresh microbit project", () => {
    const project = { [MICROBIT_V2_TARGET_COORDINATE]: MICROBIT_V2_TARGET_REFERENCE };
    // Position is a transitive sub-dependency of the featured chassis libraries;
    // the canonical listing rule keeps such sub-deps unlisted. It is redirected
    // by the transport-flip move, never surfaced as its own offer or card.
    const offers = buildMicrobitCatalogOffers(project, appEmbedRecord());
    assert.equal(
      offers.find((offer) => offer.coordinate === CODAL_POSITION_EXT_COORDINATE),
      undefined,
      "Position is not offered"
    );
    const entries = buildMicrobitExtensionEntries(project, appEmbedRecord());
    assert.equal(
      entries.find((entry) => entry.coordinate === CODAL_POSITION_EXT_COORDINATE),
      undefined,
      "Position is not an entry card"
    );
  });

  test("Position still resolves through the move and registers its Position struct type", () => {
    const harness = buildExtensionTestHarness({ install: [CODAL_POSITION_EXT_COORDINATE] });
    assert.ok(harness.bundle, "the resolved closure compiles a bundle");
    // positionTypeId throws unless the Position type registered from the gh: fixture content.
    assert.ok(harness.positionTypeId(), "the Position struct type is registered from the fixture-served gh: content");
  });
});

describe("Position graduation -- transitive dep-ref redirect", () => {
  // A dependency saved while Position was bundled still declares the
  // embedded: reference; only the move can resolve it now.
  const DEPENDENT = "example-org/position-dependent";
  const dependent = dependentExtension(DEPENDENT, {
    [CODAL_POSITION_EXT_COORDINATE]: `embedded:${CODAL_POSITION_EXT_COORDINATE}`,
  });
  const project = {
    [MICROBIT_V2_LIB_COORDINATE]: MICROBIT_V2_LIB_REFERENCE,
    [DEPENDENT]: `embedded:${DEPENDENT}`,
  };

  test("without a move, the embedded Position dependency does not resolve", () => {
    const resolved = resolveProjectExtensions(project, { embedded: [...appEmbedRecord(), dependent] });
    const origins = resolved.dependencyMounts.map((mount) => mount.namespace);
    assert.equal(
      origins.includes(CODAL_POSITION_EXT_COORDINATE),
      false,
      "with the embedded copy gone and no move, Position is unresolved"
    );
  });

  test("with the move, the embedded Position dependency resolves through the published gh: ref", () => {
    const resolved = resolveProjectExtensions(project, {
      ...sourcesWithMove(),
      embedded: [...appEmbedRecord(), dependent],
    });
    const origin = resolved.origins.find((entry) => entry.origin === CODAL_POSITION_EXT_COORDINATE);
    assert.ok(origin, "Position enters the closure through the move");
    assert.equal(origin.reference, CODAL_POSITION_GH_REF, "Position resolved through the gh: ref, not an embedded one");
  });
});

describe("chassis rename -- a dep-ref to the retired coordinate follows the move at depth", () => {
  // A dependency saved while the Cutebot library was bundled under its retired
  // coordinate: its embedded dep-ref is captured by the rename move, so the
  // library resolves under the NEW coordinate at the catalog pin.
  const DEPENDENT = "example-org/cutebot-dependent";
  const dependent = dependentExtension(DEPENDENT, {
    [RETIRED_CUTEBOT_COORDINATE]: `embedded:${RETIRED_CUTEBOT_COORDINATE}`,
  });
  const project = {
    [MICROBIT_V2_LIB_COORDINATE]: MICROBIT_V2_LIB_REFERENCE,
    [DEPENDENT]: `embedded:${DEPENDENT}`,
  };

  test("without the move, the retired embedded chassis dependency does not resolve", () => {
    const resolved = resolveProjectExtensions(project, { embedded: [...appEmbedRecord(), dependent] });
    const origins = resolved.dependencyMounts.map((mount) => mount.namespace);
    assert.equal(origins.includes(RETIRED_CUTEBOT_COORDINATE), false);
    assert.equal(origins.includes(CUTEBOT_EXT_COORDINATE), false);
  });

  test("with the move, the dep-ref resolves under the NEW coordinate at the catalog pin, and its Position dep heals", () => {
    const resolved = resolveProjectExtensions(project, {
      ...sourcesWithMove(),
      embedded: [...appEmbedRecord(), dependent],
    });
    const cutebot = resolved.origins.find((entry) => entry.origin === CUTEBOT_EXT_COORDINATE);
    assert.ok(cutebot, "the chassis library enters the closure under its new coordinate");
    assert.equal(cutebot.reference, CUTEBOT_GH_REF);
    assert.equal(
      resolved.origins.find((entry) => entry.origin === RETIRED_CUTEBOT_COORDINATE),
      undefined,
      "the retired coordinate is not in the closure"
    );
    const position = resolved.origins.find((entry) => entry.origin === CODAL_POSITION_EXT_COORDINATE);
    assert.ok(position, "the chassis library's Position dependency resolves");
    assert.equal(position.reference, CODAL_POSITION_VERSION_REF, "at the published manifest's version-form pin");
  });
});

describe("Position graduation -- a newer published pin is not dragged to the flip pin", () => {
  const NEWER_SHA = "9999999999999999999999999999999999999999";
  const newerReference = `gh:${CODAL_POSITION_EXT_COORDINATE}@${NEWER_SHA}`;
  const DEPENDENT = "example-org/newer-dependent";

  test("a transitive dependency pinned newer than the flip resolves at its own pin", () => {
    const dependent = dependentExtension(DEPENDENT, { [CODAL_POSITION_EXT_COORDINATE]: newerReference });
    const newerContent = publishedLibraryFetched.get(CODAL_POSITION_GH_REF);
    assert.ok(newerContent);
    const fetched = new Map([...publishedLibraryFetched, [newerReference, newerContent]]);
    const resolved = resolveProjectExtensions(
      { [MICROBIT_V2_LIB_COORDINATE]: MICROBIT_V2_LIB_REFERENCE, [DEPENDENT]: `embedded:${DEPENDENT}` },
      { embedded: [...appEmbedRecord(), dependent], fetched, moves: microbitLibraryCatalogMoves }
    );
    const origin = resolved.origins.find((entry) => entry.origin === CODAL_POSITION_EXT_COORDINATE);
    assert.ok(origin, "Position resolves");
    assert.equal(origin.reference, newerReference, "the newer gh pin survives the flip untouched");
  });
});

describe("Position graduation -- embedded transport gone", () => {
  test("Position is not carried in the embed record", () => {
    const origins = appEmbedRecord().map((extension) => extension.canonicalOrigin);
    assert.equal(origins.includes(CODAL_POSITION_EXT_COORDINATE), false);
  });

  test("an embedded Position reference does not resolve, while the gh: reference does", () => {
    const embeddedProject = {
      [MICROBIT_V2_LIB_COORDINATE]: MICROBIT_V2_LIB_REFERENCE,
      [CODAL_POSITION_EXT_COORDINATE]: `embedded:${CODAL_POSITION_EXT_COORDINATE}`,
    };
    const embeddedResolved = resolveProjectExtensions(embeddedProject, { embedded: appEmbedRecord() });
    assert.equal(
      embeddedResolved.dependencyMounts.map((mount) => mount.namespace).includes(CODAL_POSITION_EXT_COORDINATE),
      false,
      "the embedded transport no longer serves Position"
    );

    const ghProject = {
      [MICROBIT_V2_LIB_COORDINATE]: MICROBIT_V2_LIB_REFERENCE,
      [CODAL_POSITION_EXT_COORDINATE]: CODAL_POSITION_GH_REF,
    };
    const ghResolved = resolveProjectExtensions(ghProject, {
      embedded: appEmbedRecord(),
      fetched: publishedLibraryFetched,
    });
    assert.ok(
      ghResolved.origins.find((entry) => entry.origin === CODAL_POSITION_EXT_COORDINATE),
      "only the gh: path resolves Position"
    );
  });
});

/** Installs a stub `localStorage` for the duration of a test; returns a restore function. */
function installLocalStorage(): () => void {
  const original = Object.getOwnPropertyDescriptor(globalThis, "localStorage");
  Object.defineProperty(globalThis, "localStorage", {
    configurable: true,
    value: {
      length: 0,
      clear() {},
      getItem() {
        return null;
      },
      key() {
        return null;
      },
      removeItem() {},
      setItem() {},
    },
  });
  return () => {
    if (original) {
      Object.defineProperty(globalThis, "localStorage", original);
    } else {
      Reflect.deleteProperty(globalThis, "localStorage");
    }
  };
}

/**
 * A project manager over an in-memory file system seeded with the given
 * top-level extensions map, as a project saved before graduation. `appData`
 * carries the project's persisted store (installed-extension snapshots, brains);
 * passing a shared map across two managers mirrors two loads of the same saved
 * project. `updateActive` merges the applied extensions map, mirroring the
 * install transaction's manifest write.
 */
function seededProjectManager(
  extensions: Record<string, string>,
  filesystem: ProjectFileSystem,
  appData: Map<string, string> = new Map()
): ProjectManager {
  let activeProject: ActiveProject = {
    manifest: {
      id: "p1",
      projectCollectionId: "c1",
      name: "p1",
      version: "0.1.0",
      description: "",
      createdAt: 1,
      updatedAt: 1,
      extensions,
    },
    filesystem,
  } as ActiveProject;
  return {
    get activeProject(): ActiveProject {
      return activeProject;
    },
    activeProjectCollection: { projectCollectionId: "c1", name: "c", createdAt: 1, updatedAt: 1 },
    async init(): Promise<void> {},
    async getProjectCollectionState(): Promise<{ access: "ready" }> {
      return { access: "ready" };
    },
    async ensureDefaultProject(): Promise<void> {},
    async updateActive(updates: { extensions?: Record<string, string> }): Promise<void> {
      activeProject = { manifest: { ...activeProject.manifest, ...updates }, filesystem } as ActiveProject;
    },
    async saveAppData(key: string, data: string): Promise<void> {
      appData.set(key, data);
    },
    async loadAppData(key: string): Promise<string | undefined> {
      return appData.get(key);
    },
    async deleteAppData(key: string): Promise<void> {
      appData.delete(key);
    },
    dispose(): void {},
  } as unknown as ProjectManager;
}

/** A published-library fixture transport that counts its `fetchFile` calls, so a test can assert a load did or did not fetch. */
function countingTransport(): {
  transport: ExtensionFetchTransport;
  fetchCount: () => number;
} {
  const inner = createPublishedLibraryFixtureTransport();
  let calls = 0;
  return {
    fetchCount: () => calls,
    transport: {
      async fetchFile(owner, repo, pin, path) {
        calls += 1;
        return inner.fetchFile(owner, repo, pin, path);
      },
      resolveBranch: inner.resolveBranch.bind(inner),
      listVersionTags: inner.listVersionTags.bind(inner),
    },
  };
}

/** The app's host over the seeded project, wired with the catalog moves and a given published-library fetch transport. */
function makeHost(
  projectManager: ProjectManager,
  transport: ExtensionFetchTransport = createPublishedLibraryFixtureTransport()
): AppEnvironmentHost {
  const profile = getWodalDeviceProfile(WodalDeviceProfileId.MICROBIT_V2);
  return new AppEnvironmentHost({
    projectManager,
    modules: [coreModule(), createWodalSharedModule(), profile.createWendooModule()],
    numerics: createProfileNumerics(profile.numberPrecision),
    mounts: [],
    embeddedExtensions: appEmbedRecord(),
    catalogMoves: microbitLibraryCatalogMoves,
    extensionFetchTransport: transport,
  });
}

describe("Position graduation -- saved-project migration (top-level)", () => {
  test("a top-level embedded Position reference migrates to the gh: ref on load and still resolves", async () => {
    const restore = installLocalStorage();
    const host = makeHost(
      seededProjectManager(
        { ...microbitDefaultExtensions, [CODAL_POSITION_EXT_COORDINATE]: `embedded:${CODAL_POSITION_EXT_COORDINATE}` },
        createInMemoryProjectFileSystem()
      )
    );
    try {
      await host.initialize("p1");

      const extensions = host.activeProjectManifest?.extensions ?? {};
      assert.equal(
        extensions[CODAL_POSITION_EXT_COORDINATE],
        CODAL_POSITION_GH_REF,
        "the load-time move rewrite replaced the embedded reference with the gh: reference"
      );

      const migratedResolved = resolveProjectExtensions(extensions, sourcesWithMove());
      assert.ok(
        migratedResolved.origins.find((entry) => entry.origin === CODAL_POSITION_EXT_COORDINATE),
        "the migrated project resolves Position through the gh: ref"
      );
    } finally {
      host.dispose();
      restore();
    }
  });
});

describe("chassis rename -- a saved retired-coordinate reference migrates and heals on load", () => {
  test("a project holding the retired embedded Cutebot reference rewrites to the new gh: pin and fetches its closure", async () => {
    const restore = installLocalStorage();
    // Seeded exactly as a project saved while the chassis library was bundled:
    // the retired coordinate holds an embedded reference, and no snapshot was
    // ever persisted.
    const { transport, fetchCount } = countingTransport();
    const host = makeHost(
      seededProjectManager(
        { ...microbitDefaultExtensions, [RETIRED_CUTEBOT_COORDINATE]: `embedded:${RETIRED_CUTEBOT_COORDINATE}` },
        createInMemoryProjectFileSystem()
      ),
      transport
    );
    try {
      // Plain load: initialize only, no install transaction.
      await host.initialize("p1");

      assert.ok(fetchCount() > 0, "the load fetched the moved chassis content through the transport");

      // The manifest holds only the new identity.
      const extensions = host.activeProjectManifest?.extensions ?? {};
      assert.equal(RETIRED_CUTEBOT_COORDINATE in extensions, false, "the retired coordinate left the manifest");
      assert.equal(extensions[CUTEBOT_EXT_COORDINATE], CUTEBOT_GH_REF, "the new coordinate holds the catalog pin");

      // The fetched snapshots persisted under the new identities: the chassis
      // at its catalog pin, and its Position dependency at the published
      // manifest's version-form pin.
      const metadata = host.getInstalledExtensionMetadata();
      assert.equal(metadata[CUTEBOT_EXT_COORDINATE]?.reference, CUTEBOT_GH_REF);
      assert.equal(metadata[RETIRED_CUTEBOT_COORDINATE], undefined);
      assert.equal(metadata[CODAL_POSITION_EXT_COORDINATE]?.reference, CODAL_POSITION_VERSION_REF);

      const installed = host.installedLibraries.map((library) => library.coordinate);
      assert.ok(installed.includes(CUTEBOT_EXT_COORDINATE), "the chassis resolved into the installed closure");
      assert.ok(installed.includes(CODAL_POSITION_EXT_COORDINATE), "Position resolved into the installed closure");
    } finally {
      host.dispose();
      restore();
    }
  });

  test("a second load of an already-migrated project performs zero fetches", async () => {
    const restore = installLocalStorage();
    const store = new Map<string, string>();
    const filesystemFactory = createInMemoryProjectFileSystem;
    const extensions = {
      ...microbitDefaultExtensions,
      [RETIRED_CUTEBOT_COORDINATE]: `embedded:${RETIRED_CUTEBOT_COORDINATE}`,
    };

    const first = countingTransport();
    const firstHost = makeHost(seededProjectManager(extensions, filesystemFactory(), store), first.transport);
    let migrated: Record<string, string>;
    try {
      await firstHost.initialize("p1");
      assert.ok(first.fetchCount() > 0, "the first load migrates by fetching the moved content");
      migrated = { ...(firstHost.activeProjectManifest?.extensions ?? {}) };
    } finally {
      firstHost.dispose();
    }

    // Second load over the SAME persisted store, seeded with the migrated
    // manifest: every snapshot is present, so the load is a cache hit and the
    // manifest does not change again.
    const second = countingTransport();
    const secondHost = makeHost(seededProjectManager(migrated, filesystemFactory(), store), second.transport);
    try {
      await secondHost.initialize("p1");
      assert.equal(second.fetchCount(), 0, "the already-persisted moved content is not refetched");
      assert.deepEqual(
        secondHost.activeProjectManifest?.extensions,
        migrated,
        "the migrated manifest is stable across loads"
      );
      const installed = secondHost.installedLibraries.map((library) => library.coordinate);
      assert.ok(installed.includes(CUTEBOT_EXT_COORDINATE), "the chassis still resolves from the cached snapshot");
      assert.ok(installed.includes(CODAL_POSITION_EXT_COORDINATE), "Position still resolves from the cached snapshot");
    } finally {
      secondHost.dispose();
      restore();
    }
  });
});
