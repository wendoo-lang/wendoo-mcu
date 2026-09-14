import "fake-indexeddb/auto";
import assert from "node:assert/strict";
import { describe, test } from "node:test";
import { fileURLToPath } from "node:url";
import type { ExtensionFetchTransport } from "@wendoo/app-host";
import {
  createIdbProjectStore,
  DEFAULT_PROJECT_NAME,
  importProjectDocument,
  type ProjectFileSnapshot,
  ProjectManager,
} from "@wendoo/app-host";
import type { EmbeddedExtension } from "@wendoo/bridge-app";
import { AppEnvironmentHost, CatalogMoveWarningCode, INSTALLED_EXTENSIONS_APP_DATA_KEY } from "@wendoo/bridge-app";
import { buildEmbeddedExtensionFromDir } from "@wendoo/bridge-app/node";
import { coreModule } from "@wendoo/core/app";
import type { IBrainDef, IBrainRuleDef, IBrainTileDef } from "@wendoo/core/brain";
import { createProfileNumerics } from "@wendoo/core/runtime";
import { isCompilerControlledPath, type WorkspaceCompileResult } from "@wendoo/ts-compiler";
import { createWodalSharedModule, getWodalDeviceProfile, WodalDeviceProfileId } from "@wendoo/wodal";
import { microbitLibraryCatalogMoves } from "../services/microbit-extension-browser";
import {
  CODAL_LIB_COORDINATE,
  CODAL_POSITION_EXT_COORDINATE,
  CORE_LIB_COORDINATE,
  CUTEBOT_EXT_COORDINATE,
  MICROBIT_V2_LIB_COORDINATE,
  MICROBIT_V2_TARGET_COORDINATE,
  MICROBIT_V2_TARGET_REFERENCE,
  microbitDefaultExtensions,
  YAHBOOM_GAMEPAD_EXT_COORDINATE,
} from "../services/microbit-extension-coordinates";
import { MICROBIT_SIM_APP_CHUNK_KEY, translateMicrobitSimAppChunk } from "../services/project-io";
import { unresolvedLibraryCoordinates } from "../services/resolution-warnings";
import {
  CODAL_POSITION_VERSION_REF,
  CUTEBOT_GH_REF,
  createPublishedLibraryFixtureTransport,
  YAHBOOM_GAMEPAD_GH_REF,
} from "./published-library-fixtures";

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
 * The retired embedded coordinates a pre-rename project's manifest and brains
 * hold. The bundled catalog's rename moves migrate them to the published
 * coordinates on load.
 */
const LEGACY_CUTEBOT_COORDINATE = "wendoo-lang/lib-microbit-cutebot";
const LEGACY_YAHBOOM_COORDINATE = "wendoo-lang/lib-microbit-yahboom-gamepad";

/**
 * The app's full embed record: the same four coordinate/directory registrations
 * as `vite/embedded-extensions.mjs`, assembled through
 * {@link buildEmbeddedExtensionFromDir}, the single content-assembly path the
 * app's Vite provider also uses. The feature libraries are not bundled.
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

/**
 * The legacy project's top-level extensions map: the pre-rename shape, where
 * the runnable target and the two chassis add-ons are the only entries, each
 * chassis under its retired coordinate at an `embedded:` reference.
 */
const LEGACY_EXTENSIONS: Readonly<Record<string, string>> = {
  [MICROBIT_V2_TARGET_COORDINATE]: MICROBIT_V2_TARGET_REFERENCE,
  [LEGACY_YAHBOOM_COORDINATE]: `embedded:${LEGACY_YAHBOOM_COORDINATE}`,
  [LEGACY_CUTEBOT_COORDINATE]: `embedded:${LEGACY_CUTEBOT_COORDINATE}`,
};

/** The extensions map the load's rename migration produces from {@link LEGACY_EXTENSIONS}. */
const MIGRATED_EXTENSIONS: Readonly<Record<string, string>> = {
  [MICROBIT_V2_TARGET_COORDINATE]: MICROBIT_V2_TARGET_REFERENCE,
  [YAHBOOM_GAMEPAD_EXT_COORDINATE]: YAHBOOM_GAMEPAD_GH_REF,
  [CUTEBOT_EXT_COORDINATE]: CUTEBOT_GH_REF,
};

const MY_ACTUATOR_PATH = "my-actuator/my-actuator.ts";
const MY_ACTUATOR_SOURCE = `import { Actuator } from "wendoo";
import { heart } from "@lib/wendoo-lang/lib-microbit-v2"
const icon = heart();
export default Actuator({ id: "fbkHu4V3tO8EQ8Bd", name: "my actuator", onExecute(ctx, params) {}, });
`;

/** The dependent-library files whose Position imports the load must resolve, under the migrated coordinates. */
const POSITION_DEPENDENT_PATHS: readonly string[] = [
  `.libraries/${CUTEBOT_EXT_COORDINATE}/steer.ts`,
  `.libraries/${YAHBOOM_GAMEPAD_EXT_COORDINATE}/stick-position.ts`,
  `.libraries/${YAHBOOM_GAMEPAD_EXT_COORDINATE}/decoded-stick-position.ts`,
  `.libraries/${YAHBOOM_GAMEPAD_EXT_COORDINATE}/position-to-buffer.ts`,
];

// ---------------------------------------------------------------------------
// The legacy brains: cutebot/yahboom tile instances in the persisted form the
// pre-rename editor serialized, holding the RETIRED coordinate namespaces.
// ---------------------------------------------------------------------------

/** Brain keys of the legacy project's stored brains record. */
const REMOTE_BRAIN_KEY = "gkIOi3pfMfDsy6yC";
const DRIVE_BRAIN_KEY = "6r44shQiaucjP4ar";

/**
 * A Cutebot actuator instance in the persisted form the pre-rename editor
 * serialized for a foreign-namespace library tile: the library's stable action
 * id under its retired coordinate namespace.
 */
const LEGACY_CUTEBOT_DRIVE_TILE_REF = {
  k: "action",
  area: "actuator",
  id: "zClgsu5drwdekq7r",
  ns: LEGACY_CUTEBOT_COORDINATE,
} as const;

/** A Yahboom stick sensor instance in the same persisted form. */
const LEGACY_YAHBOOM_STICK_TILE_REF = {
  k: "action",
  area: "sensor",
  id: "qyPhWctORp9bXYAc",
  ns: LEGACY_YAHBOOM_COORDINATE,
} as const;

/** The same persisted refs after the rename migration: only the namespace changes; stable id, kind, and area survive. */
const MIGRATED_CUTEBOT_DRIVE_TILE_REF = { ...LEGACY_CUTEBOT_DRIVE_TILE_REF, ns: CUTEBOT_EXT_COORDINATE } as const;
const MIGRATED_YAHBOOM_STICK_TILE_REF = {
  ...LEGACY_YAHBOOM_STICK_TILE_REF,
  ns: YAHBOOM_GAMEPAD_EXT_COORDINATE,
} as const;

/** Runtime tile ids the migrated refs decode to: the same stable ids under the published coordinates. */
const CUTEBOT_DRIVE_TILE_ID = `tile.actuator->${CUTEBOT_EXT_COORDINATE}:user.actuator.${LEGACY_CUTEBOT_DRIVE_TILE_REF.id}`;
const YAHBOOM_STICK_TILE_ID = `tile.sensor->${YAHBOOM_GAMEPAD_EXT_COORDINATE}:user.sensor.${LEGACY_YAHBOOM_STICK_TILE_REF.id}`;

/** Platform host-action tile ids, serialized as plain strings in the legacy brains. */
const RADIO_SEND_TILE_ID = "tile.actuator->microbit-v2.radio-send";
const RADIO_RECEIVE_BUFFER_TILE_ID = "tile.sensor->microbit-v2.radio-receive-buffer";

/** The legacy project's brains record, exactly as the pre-rename editor persisted it. */
function legacyBrainsRecord(): Record<string, unknown> {
  return {
    [REMOTE_BRAIN_KEY]: {
      version: 1,
      id: "ttD0r70L1Z20Sp8b",
      name: "robot remote",
      catalog: [{ version: 2, kind: "page", pageId: "ZOygVYKtp5pfVXG0", label: "Unnamed Page" }],
      pages: [
        {
          version: 2,
          pageId: "ZOygVYKtp5pfVXG0",
          name: "Unnamed Page",
          rules: [
            { version: 1, when: [], do: [RADIO_SEND_TILE_ID], children: [] },
            {
              version: 1,
              when: [LEGACY_YAHBOOM_STICK_TILE_REF],
              do: [LEGACY_CUTEBOT_DRIVE_TILE_REF],
              children: [],
            },
          ],
        },
      ],
    },
    [DRIVE_BRAIN_KEY]: {
      version: 1,
      id: "ms5n5wbY5nGdb7Wg",
      name: "robot drive",
      catalog: [{ version: 2, kind: "page", pageId: "mNdhCKxdpaw2ZvTj", label: "Unnamed Page" }],
      pages: [
        {
          version: 2,
          pageId: "mNdhCKxdpaw2ZvTj",
          name: "Unnamed Page",
          rules: [
            { version: 1, when: [RADIO_RECEIVE_BUFFER_TILE_ID], do: [LEGACY_CUTEBOT_DRIVE_TILE_REF], children: [] },
            { version: 1, when: [], do: [], children: [] },
          ],
        },
      ],
    },
  };
}

/** Wrap a transport with a fetch-call counter, so a load's fetch activity is observable. */
function countingTransport(inner: ExtensionFetchTransport = createPublishedLibraryFixtureTransport()): {
  transport: ExtensionFetchTransport;
  fetchCount: () => number;
} {
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

let storeCounter = 0;

/**
 * The host exactly as `MicrobitSimEnvironmentStore.create()` constructs it in
 * browser mode, with the headless substitutions stated where they occur: the
 * IDB store runs over fake-indexeddb, the jsDelivr transport is replaced by
 * the published-library fetch fixture (no network in tests), and the Web Locks
 * project lock and the bridge URL are omitted (browser-only surfaces the load
 * path never consumes before `initBridge`).
 */
async function makeProductionShapedHost(
  transport: ExtensionFetchTransport,
  storeName = `legacy-platform-load-${storeCounter++}`
): Promise<{
  host: AppEnvironmentHost;
  projectManager: ProjectManager;
  storeName: string;
  lastCompile: () => WorkspaceCompileResult | undefined;
}> {
  const activeProfile = getWodalDeviceProfile(WodalDeviceProfileId.MICROBIT_V2);
  const projectStore = await createIdbProjectStore(storeName);
  let lastCompile: WorkspaceCompileResult | undefined;
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
    extensionFetchTransport: transport,
    catalogMoves: microbitLibraryCatalogMoves,
    rng: { next: () => Math.random() },
    onDidCompile: (result) => {
      lastCompile = result;
    },
  });
  return { host, projectManager, storeName, lastCompile: () => lastCompile };
}

/**
 * Seed the legacy "cuterbot" project into the durable store: the three-entry
 * pre-rename manifest extensions map, the user-content actuator file, and the
 * legacy brains record. `appData` seeds further persisted app-data records.
 */
async function seedLegacyProject(
  projectManager: ProjectManager,
  appData: Record<string, string> = {}
): Promise<string> {
  const snapshot: ProjectFileSnapshot = new Map([
    [MY_ACTUATOR_PATH, { kind: "file", content: MY_ACTUATOR_SOURCE, etag: "e1", isReadonly: false }],
  ]);
  const created = await projectManager.createFromSnapshot(
    "cuterbot",
    "",
    snapshot,
    { brains: JSON.stringify(legacyBrainsRecord()), ...appData },
    undefined,
    LEGACY_EXTENSIONS,
    "0.1.0"
  );
  return created.id;
}

/** Error-severity diagnostic entries recorded for `path` in the compile result. */
function errorsAt(result: WorkspaceCompileResult | undefined, path: string): readonly { code: string }[] {
  return (result?.files.get(path) ?? []).filter((entry) => entry.severity === "error");
}

/** One production-shaped load of the legacy project through the picker's path: startup, seed, switch. */
async function loadLegacyProject(
  appData: Record<string, string> = {},
  transport?: ExtensionFetchTransport
): Promise<{
  host: AppEnvironmentHost;
  projectManager: ProjectManager;
  storeName: string;
  legacyId: string;
  fetchesDuringSwitch: number;
  result: WorkspaceCompileResult | undefined;
}> {
  const counting = countingTransport(transport ?? createPublishedLibraryFixtureTransport());
  const { host, projectManager, storeName, lastCompile } = await makeProductionShapedHost(counting.transport);
  // App startup: opens/creates the default project, as store.initialize does.
  await host.initialize(DEFAULT_PROJECT_NAME);
  const startupFetches = counting.fetchCount();
  const legacyId = await seedLegacyProject(projectManager, appData);
  // The picker's load path: store.switchProject delegates here directly.
  await host.switchProject(legacyId);
  return {
    host,
    projectManager,
    storeName,
    legacyId,
    fetchesDuringSwitch: counting.fetchCount() - startupFetches,
    result: lastCompile(),
  };
}

/** The stored brain record's rule arrays, parsed from the active project's persisted brains record. */
async function storedRules(
  projectManager: ProjectManager,
  brainKey: string
): Promise<readonly { when: unknown[]; do: unknown[] }[]> {
  const raw = await projectManager.loadAppData("brains");
  assert.ok(raw, "the project persisted a brains record");
  const record = JSON.parse(raw) as Record<string, { pages: { rules: { when: unknown[]; do: unknown[] }[] }[] }>;
  return record[brainKey].pages[0].rules;
}

/** Every tile placed on a rule of the brain, when and do sides, child rules included. */
function collectRuleTiles(brainDef: IBrainDef): IBrainTileDef[] {
  const tiles: IBrainTileDef[] = [];
  const visitRule = (rule: IBrainRuleDef): void => {
    rule
      .when()
      .tiles()
      .forEach((tile) => {
        tiles.push(tile);
      });
    rule
      .do()
      .tiles()
      .forEach((tile) => {
        tiles.push(tile);
      });
    const children = rule.children();
    for (let i = 0; i < children.size(); i++) {
      visitRule(children.get(i)!);
    }
  };
  const pages = brainDef.pages();
  for (let i = 0; i < pages.size(); i++) {
    const rules = pages.get(i)!.children();
    for (let j = 0; j < rules.size(); j++) {
      visitRule(rules.get(j)!);
    }
  }
  return tiles;
}

/** The kinds of the brain's placed tiles matching `tileId`, in placement order. */
function kindsOfTile(brainDef: IBrainDef, tileId: string): string[] {
  return collectRuleTiles(brainDef)
    .filter((tile) => tile.tileId === tileId)
    .map((tile) => tile.kind);
}

/**
 * Assert the persisted project holds no retired coordinate anywhere: the
 * manifest extensions map, the brains record, and the installed-extension
 * snapshots record.
 */
async function assertNoRetiredCoordinateRemains(host: AppEnvironmentHost, projectManager: ProjectManager) {
  const persistedForms = [
    JSON.stringify(host.activeProjectManifest?.extensions ?? {}),
    (await projectManager.loadAppData("brains")) ?? "",
    (await projectManager.loadAppData(INSTALLED_EXTENSIONS_APP_DATA_KEY)) ?? "",
  ];
  for (const form of persistedForms) {
    assert.equal(form.includes(LEGACY_CUTEBOT_COORDINATE), false, `a retired cutebot coordinate remains: ${form}`);
    assert.equal(form.includes(LEGACY_YAHBOOM_COORDINATE), false, `a retired yahboom coordinate remains: ${form}`);
  }
}

describe("legacy project load -- production wiring", () => {
  test("the load migrates both chassis entries to their published coordinates and heals the closure by fetching", async () => {
    const { host, projectManager, fetchesDuringSwitch, result } = await loadLegacyProject();
    try {
      // The rename moves rewrote the manifest to the new gh: pins, and the
      // load fetched the published content this project never persisted.
      assert.ok(fetchesDuringSwitch > 0, "the load fetched the moved chassis content through the transport");
      assert.deepEqual(host.activeProjectManifest?.extensions, MIGRATED_EXTENSIONS);

      // The fetched snapshots persisted under the new identities; the chassis
      // libraries' Position dependency healed at its version-form pin.
      const metadata = host.getInstalledExtensionMetadata();
      assert.equal(metadata[CUTEBOT_EXT_COORDINATE]?.reference, CUTEBOT_GH_REF);
      assert.equal(metadata[YAHBOOM_GAMEPAD_EXT_COORDINATE]?.reference, YAHBOOM_GAMEPAD_GH_REF);
      assert.equal(metadata[CODAL_POSITION_EXT_COORDINATE]?.reference, CODAL_POSITION_VERSION_REF);

      const installed = host.installedLibraries.map((library) => library.coordinate);
      assert.ok(installed.includes(CUTEBOT_EXT_COORDINATE), "cutebot resolved into the installed closure");
      assert.ok(installed.includes(YAHBOOM_GAMEPAD_EXT_COORDINATE), "yahboom resolved into the installed closure");
      assert.ok(installed.includes(CODAL_POSITION_EXT_COORDINATE), "Position resolved into the installed closure");

      assert.ok(result, "expected a compile result for the legacy project");
      for (const path of POSITION_DEPENDENT_PATHS) {
        assert.deepEqual(
          errorsAt(result, path).map((entry) => entry.code),
          [],
          `expected no error diagnostics in ${path}`
        );
      }
      await assertNoRetiredCoordinateRemains(host, projectManager);
    } finally {
      host.dispose();
    }
  });

  test("saved-brain namespaces rewrite with the manifest, and the instances resolve to the migrated tiles", async () => {
    const { host, projectManager, result } = await loadLegacyProject();
    try {
      // Persisted form: the brains record's tile refs adopted the new
      // namespaces in the same load that rewrote the manifest.
      const remoteRules = await storedRules(projectManager, REMOTE_BRAIN_KEY);
      assert.deepEqual(remoteRules[1].when, [MIGRATED_YAHBOOM_STICK_TILE_REF]);
      assert.deepEqual(remoteRules[1].do, [MIGRATED_CUTEBOT_DRIVE_TILE_REF]);
      const driveRules = await storedRules(projectManager, DRIVE_BRAIN_KEY);
      assert.deepEqual(driveRules[0].when, [RADIO_RECEIVE_BUFFER_TILE_ID]);
      assert.deepEqual(driveRules[0].do, [MIGRATED_CUTEBOT_DRIVE_TILE_REF]);

      // Live form: the same load registers the migrated libraries' tiles, and
      // the rewritten instances resolve to them -- not placeholders.
      const bundleTileIds = (result?.bundle?.tiles ?? []).map((tile) => tile.tileId);
      assert.ok(bundleTileIds.includes(CUTEBOT_DRIVE_TILE_ID), "the cutebot tile registered under the new namespace");
      assert.ok(bundleTileIds.includes(YAHBOOM_STICK_TILE_ID), "the yahboom tile registered under the new namespace");
      const remote = host.getCachedBrain(REMOTE_BRAIN_KEY);
      const drive = host.getCachedBrain(DRIVE_BRAIN_KEY);
      assert.ok(remote, "the remote brain deserialized into the cache");
      assert.ok(drive, "the drive brain deserialized into the cache");
      assert.deepEqual(kindsOfTile(remote, CUTEBOT_DRIVE_TILE_ID), ["actuator"]);
      assert.deepEqual(kindsOfTile(remote, YAHBOOM_STICK_TILE_ID), ["sensor"]);
      assert.deepEqual(kindsOfTile(drive, CUTEBOT_DRIVE_TILE_ID), ["actuator"]);

      await assertNoRetiredCoordinateRemains(host, projectManager);
    } finally {
      host.dispose();
    }
  });

  test("the load persists no platform layer or transitive dependency into the manifest extensions map", async () => {
    const { host } = await loadLegacyProject();
    try {
      // The rename rewrote the two chassis keys; nothing else joined the map.
      assert.deepEqual(
        Object.keys(host.activeProjectManifest?.extensions ?? {}).sort(),
        Object.keys(MIGRATED_EXTENSIONS).sort(),
        "the manifest extensions map holds exactly the migrated entries"
      );
    } finally {
      host.dispose();
    }
  });

  // The platform stack reached through the target's `targets` edges joins the
  // project's direct dependencies, so user content may import any of its
  // layers.
  test("user content importing the transitively-reached platform stdlib compiles", async () => {
    const { host, result } = await loadLegacyProject();
    try {
      assert.ok(result, "expected a compile result for the legacy project");
      // The stack DID materialize: the stdlib layer is in the resolved closure.
      const installed = host.installedLibraries.map((library) => library.coordinate);
      assert.ok(installed.includes(MICROBIT_V2_LIB_COORDINATE), "the stdlib layer resolved into the closure");
      // The user-content import of that same layer must resolve too.
      assert.deepEqual(
        errorsAt(result, MY_ACTUATOR_PATH).map((entry) => entry.code),
        [],
        `expected no error diagnostics in ${MY_ACTUATOR_PATH}`
      );
    } finally {
      host.dispose();
    }
  });

  // A persisted installed-extensions record whose reference matches a needed
  // pin but whose files carry no parseable manifest is unusable content: the
  // heal walk treats it as missing, refetches, and replaces the record, with
  // zero user action.
  test("a corrupt stored Position snapshot is detected and re-fetched instead of silently mounting nothing", async () => {
    const poisonedSnapshots = JSON.stringify({
      [CODAL_POSITION_EXT_COORDINATE]: {
        reference: CODAL_POSITION_VERSION_REF,
        specifier: "0.1.4",
        files: {},
      },
    });
    const { host, fetchesDuringSwitch, result } = await loadLegacyProject({
      [INSTALLED_EXTENSIONS_APP_DATA_KEY]: poisonedSnapshots,
    });
    try {
      assert.ok(result, "expected a compile result for the legacy project");
      assert.ok(fetchesDuringSwitch > 0, "the load re-fetched the unusable stored Position content");
      for (const path of POSITION_DEPENDENT_PATHS) {
        assert.deepEqual(
          errorsAt(result, path).map((entry) => entry.code),
          [],
          `expected no error diagnostics in ${path}`
        );
      }
    } finally {
      host.dispose();
    }
  });

  test("a reload of the migrated project performs zero fetches and rewrites nothing", async () => {
    const first = await loadLegacyProject();
    const migratedManifest = { ...(first.host.activeProjectManifest?.extensions ?? {}) };
    const migratedBrains = await first.projectManager.loadAppData("brains");
    first.host.dispose();

    // A fresh host over the SAME durable store, as an app restart: startup on
    // the default project, then the picker switch to the migrated project.
    const counting = countingTransport();
    const { host, projectManager } = await makeProductionShapedHost(counting.transport, first.storeName);
    try {
      await host.initialize(DEFAULT_PROJECT_NAME);
      const startupFetches = counting.fetchCount();
      await host.switchProject(first.legacyId);
      assert.equal(counting.fetchCount() - startupFetches, 0, "the reload reaches the transport for nothing");
      assert.deepEqual(host.activeProjectManifest?.extensions, migratedManifest, "the manifest is unchanged");
      assert.equal(await projectManager.loadAppData("brains"), migratedBrains, "the brains record is unchanged");
    } finally {
      host.dispose();
    }
  });
});

describe("legacy project load -- one refused move writes nothing while the other completes", () => {
  test("a cutebot-only outage skips the cutebot move atomically, migrates yahboom, and heals on the next load", async () => {
    const cutebotOutage = createPublishedLibraryFixtureTransport({ refuse: new Set([CUTEBOT_EXT_COORDINATE]) });
    const first = await loadLegacyProject({}, cutebotOutage);
    try {
      // The yahboom unit committed: manifest and brains adopted its new identity.
      const extensions = first.host.activeProjectManifest?.extensions ?? {};
      assert.equal(extensions[YAHBOOM_GAMEPAD_EXT_COORDINATE], YAHBOOM_GAMEPAD_GH_REF);
      assert.equal(LEGACY_YAHBOOM_COORDINATE in extensions, false);

      // The cutebot unit wrote NOTHING: its manifest entry and its brain
      // namespaces are untouched together.
      assert.equal(extensions[LEGACY_CUTEBOT_COORDINATE], `embedded:${LEGACY_CUTEBOT_COORDINATE}`);
      assert.equal(CUTEBOT_EXT_COORDINATE in extensions, false);
      const remoteRules = await storedRules(first.projectManager, REMOTE_BRAIN_KEY);
      assert.deepEqual(remoteRules[1].when, [MIGRATED_YAHBOOM_STICK_TILE_REF], "the yahboom brain refs migrated");
      assert.deepEqual(remoteRules[1].do, [LEGACY_CUTEBOT_DRIVE_TILE_REF], "the cutebot brain refs stayed legacy");

      // The failed unit surfaces the stable-coded warning the banner reads.
      const warnings = first.host.getResolutionWarningsSnapshot();
      assert.ok(
        warnings.some(
          (warning) =>
            warning.kind === "catalog-move-failed" &&
            warning.code === CatalogMoveWarningCode.FETCH_FAILED &&
            warning.origin === LEGACY_CUTEBOT_COORDINATE
        ),
        `the refused move records its stable-coded warning: ${JSON.stringify(warnings)}`
      );
      assert.deepEqual(unresolvedLibraryCoordinates(warnings), [LEGACY_CUTEBOT_COORDINATE]);
    } finally {
      first.host.dispose();
    }

    // Next load with a healthy transport: the retried cutebot move completes
    // and the project holds only the new identities.
    const { host, projectManager } = await makeProductionShapedHost(
      createPublishedLibraryFixtureTransport(),
      first.storeName
    );
    try {
      await host.initialize(DEFAULT_PROJECT_NAME);
      await host.switchProject(first.legacyId);
      assert.deepEqual(host.activeProjectManifest?.extensions, MIGRATED_EXTENSIONS);
      const remoteRules = await storedRules(projectManager, REMOTE_BRAIN_KEY);
      assert.deepEqual(remoteRules[1].when, [MIGRATED_YAHBOOM_STICK_TILE_REF]);
      assert.deepEqual(remoteRules[1].do, [MIGRATED_CUTEBOT_DRIVE_TILE_REF]);
      await assertNoRetiredCoordinateRemains(host, projectManager);
      assert.deepEqual(unresolvedLibraryCoordinates(host.getResolutionWarningsSnapshot()), []);
    } finally {
      host.dispose();
    }
  });
});

// ---------------------------------------------------------------------------
// The .wendoo IMPORT path: a shared export document imported as a new
// project. Unlike the picker path above, the imported project's app data
// carries the export's saved brains but never the installed-extensions
// snapshots (an export is portable and omits them), so the first load after
// import always resolves before any content has been fetched.
// ---------------------------------------------------------------------------

/** The export's user-content file, importing the target coordinate (whose hostApp package lists no files). */
const STALE_TRG_IMPORT_SOURCE = `import { Actuator } from "wendoo";
import { heart } from "@lib/wendoo-lang/trg-microbit-v2"

const icon = heart();

export default Actuator({
  id: "fbkHu4V3tO8EQ8Bd",
  name: "my actuator",
  onExecute(ctx, params) {
  },
});
`;

/**
 * The user's "cuterbot" export document: the real file's manifest, brains, app
 * chunk, and contents, in the pre-rename persisted form -- retired coordinates
 * in the extensions map and retired namespaces on the brains' tile refs.
 */
function cuterbotDocumentText(): string {
  return JSON.stringify({
    format: "wendoo.project/2",
    manifest: {
      name: "cuterbot",
      version: "0.1.0",
      description: "",
      extensions: LEGACY_EXTENSIONS,
      brains: legacyBrainsRecord(),
      app: {
        [MICROBIT_SIM_APP_CHUNK_KEY]: {
          brainOrder: [REMOTE_BRAIN_KEY, DRIVE_BRAIN_KEY],
          simulator: {
            order: ["1bc5a8a8-097c-488f-a3fa-45422f0f98a1", "00897593-2fd3-4281-8e86-ff88909d570a"],
            flash: {
              "1bc5a8a8-097c-488f-a3fa-45422f0f98a1": REMOTE_BRAIN_KEY,
              "00897593-2fd3-4281-8e86-ff88909d570a": DRIVE_BRAIN_KEY,
            },
          },
        },
      },
    },
    contents: { [MY_ACTUATOR_PATH]: STALE_TRG_IMPORT_SOURCE },
  });
}

/**
 * One production-shaped import of the cuterbot export: startup on the default
 * project, `importProjectDocument` with the app's chunk translator, then the
 * switch to the created project -- the exact sequence of
 * `MicrobitSimEnvironmentStore.importProject`.
 */
async function importCuterbotProject(
  transport: ExtensionFetchTransport = createPublishedLibraryFixtureTransport()
): Promise<{
  host: AppEnvironmentHost;
  projectManager: ProjectManager;
  defaultProjectId: string;
  importedProjectId: string;
  result: WorkspaceCompileResult | undefined;
}> {
  const { host, projectManager, lastCompile } = await makeProductionShapedHost(transport);
  await host.initialize(DEFAULT_PROJECT_NAME);
  const defaultProjectId = projectManager.activeProject!.manifest.id;
  const file = new File([cuterbotDocumentText()], "cuterbot.wendoo");
  const imported = await importProjectDocument(file, MICROBIT_SIM_APP_CHUNK_KEY, projectManager, {
    appChunkCallback: translateMicrobitSimAppChunk,
  });
  assert.equal(imported.success, true, JSON.stringify(imported.diagnostics));
  assert.deepEqual(
    imported.diagnostics.filter((diagnostic) => diagnostic.severity === "error"),
    [],
    "the import surface reports no errors for this document"
  );
  await host.switchProject(imported.projectId!);
  return { host, projectManager, defaultProjectId, importedProjectId: imported.projectId!, result: lastCompile() };
}

/** The fixture transport behind a controllable outage: while down, every request answers not-found. */
function outageTransport(): { transport: ExtensionFetchTransport; restore: () => void } {
  const inner = createPublishedLibraryFixtureTransport();
  let down = true;
  return {
    restore: () => {
      down = false;
    },
    transport: {
      async fetchFile(owner, repo, pin, path) {
        if (down) return { ok: false, kind: "not-found" };
        return inner.fetchFile(owner, repo, pin, path);
      },
      async resolveBranch(owner, repo, branch) {
        if (down) return { ok: false, kind: "not-found" };
        return inner.resolveBranch(owner, repo, branch);
      },
      async listVersionTags(owner, repo) {
        if (down) return { ok: false, kind: "not-found" };
        return inner.listVersionTags(owner, repo);
      },
    },
  };
}

describe("cuterbot .wendoo import -- production wiring", () => {
  // The load-time migration fetches and registers the library tiles under the
  // new namespaces, so the same first load must rewrite and resolve the saved
  // brains' instances of those tiles.
  test("the imported brains' retired-namespace instances migrate and resolve on the first load", async () => {
    const { host, projectManager, result } = await importCuterbotProject();
    try {
      // Picker side: the load's final compile registered both library tiles.
      const bundleTileIds = (result?.bundle?.tiles ?? []).map((tile) => tile.tileId);
      assert.ok(bundleTileIds.includes(CUTEBOT_DRIVE_TILE_ID), "the Cutebot tile is registered for the picker");
      assert.ok(bundleTileIds.includes(YAHBOOM_STICK_TILE_ID), "the Yahboom tile is registered for the picker");
      // Persisted side: the export's retired-namespace refs migrated.
      const rules = await storedRules(projectManager, REMOTE_BRAIN_KEY);
      assert.deepEqual(rules[1].when, [MIGRATED_YAHBOOM_STICK_TILE_REF]);
      assert.deepEqual(rules[1].do, [MIGRATED_CUTEBOT_DRIVE_TILE_REF]);
      // Brain side: the saved instances of those same tiles resolve.
      const remote = host.getCachedBrain(REMOTE_BRAIN_KEY);
      const drive = host.getCachedBrain(DRIVE_BRAIN_KEY);
      assert.ok(remote, "the imported remote brain deserialized into the cache");
      assert.ok(drive, "the imported drive brain deserialized into the cache");
      assert.deepEqual(kindsOfTile(remote, CUTEBOT_DRIVE_TILE_ID), ["actuator"]);
      assert.deepEqual(kindsOfTile(remote, YAHBOOM_STICK_TILE_ID), ["sensor"]);
      assert.deepEqual(kindsOfTile(drive, CUTEBOT_DRIVE_TILE_ID), ["actuator"]);
      await assertNoRetiredCoordinateRemains(host, projectManager);
    } finally {
      host.dispose();
    }
  });

  // While the published content is unreachable, the moves are refused, both
  // libraries' tile sets are withheld, and the saved instances load as
  // missing-tile placeholders under their retired identities. The placeholders
  // must be lossless through every persistence path -- an explicit save, a
  // save after an edit elsewhere in the same brain, and the switch-away flush
  // -- and migrate and resolve once the content heals.
  test("missing-tile placeholders survive save and edit cycles verbatim and migrate after the heal", async () => {
    const { transport, restore } = outageTransport();
    const { host, projectManager, defaultProjectId, importedProjectId } = await importCuterbotProject(transport);
    try {
      const remote = host.getCachedBrain(REMOTE_BRAIN_KEY);
      assert.ok(remote, "the imported remote brain deserialized into the cache");
      // The outage refused the renames: the saved instances are placeholders
      // holding the retired identities verbatim.
      const legacyCutebotTileId = `tile.actuator->${LEGACY_CUTEBOT_COORDINATE}:user.actuator.${LEGACY_CUTEBOT_DRIVE_TILE_REF.id}`;
      const legacyYahboomTileId = `tile.sensor->${LEGACY_YAHBOOM_COORDINATE}:user.sensor.${LEGACY_YAHBOOM_STICK_TILE_REF.id}`;
      assert.deepEqual(kindsOfTile(remote, legacyCutebotTileId), ["missing"]);
      assert.deepEqual(kindsOfTile(remote, legacyYahboomTileId), ["missing"]);

      // Write 1: an explicit save of the loaded brain.
      await host.saveBrainForKey(REMOTE_BRAIN_KEY, remote);
      let rules = await storedRules(projectManager, REMOTE_BRAIN_KEY);
      assert.deepEqual(rules[1].when, [LEGACY_YAHBOOM_STICK_TILE_REF]);
      assert.deepEqual(rules[1].do, [LEGACY_CUTEBOT_DRIVE_TILE_REF]);

      // Write 2: an edit elsewhere in the same brain -- the radio rule gains a
      // platform sensor -- then a save of the edited brain.
      const radioReceive = host.env.brainServices.edit.tiles.get(RADIO_RECEIVE_BUFFER_TILE_ID);
      assert.ok(radioReceive, "the platform radio sensor tile is registered");
      remote.pages().get(0)!.children().get(0)!.when().appendTile(radioReceive);
      await host.saveBrainForKey(REMOTE_BRAIN_KEY, remote);
      rules = await storedRules(projectManager, REMOTE_BRAIN_KEY);
      assert.deepEqual(rules[0].when, [RADIO_RECEIVE_BUFFER_TILE_ID]);
      assert.deepEqual(rules[1].when, [LEGACY_YAHBOOM_STICK_TILE_REF]);
      assert.deepEqual(rules[1].do, [LEGACY_CUTEBOT_DRIVE_TILE_REF]);

      // Write 3: the switch-away flush persists the cached placeholder brains;
      // the content heals before the switch back, which migrates the refs.
      restore();
      await host.switchProject(defaultProjectId);
      await host.switchProject(importedProjectId);
      rules = await storedRules(projectManager, REMOTE_BRAIN_KEY);
      assert.deepEqual(rules[0].when, [RADIO_RECEIVE_BUFFER_TILE_ID]);
      assert.deepEqual(rules[1].when, [MIGRATED_YAHBOOM_STICK_TILE_REF]);
      assert.deepEqual(rules[1].do, [MIGRATED_CUTEBOT_DRIVE_TILE_REF]);
      // The same persisted instances now resolve to the migrated tiles.
      const healed = host.getCachedBrain(REMOTE_BRAIN_KEY);
      assert.ok(healed, "the remote brain reloaded after the heal");
      assert.deepEqual(kindsOfTile(healed, CUTEBOT_DRIVE_TILE_ID), ["actuator"]);
      assert.deepEqual(kindsOfTile(healed, YAHBOOM_STICK_TILE_ID), ["sensor"]);
      assert.deepEqual(kindsOfTile(healed, RADIO_RECEIVE_BUFFER_TILE_ID), ["sensor"]);
    } finally {
      host.dispose();
    }
  });

  // The store's warning surface delegates to the host's resolution-warning
  // snapshot; this exercises it through the same production wiring: an outage
  // load records the stable-coded move failures, and the healed load clears
  // them.
  test("an outage load exposes the stable-coded move warnings and the healed load clears them", async () => {
    const { transport, restore } = outageTransport();
    const { host, defaultProjectId, importedProjectId } = await importCuterbotProject(transport);
    try {
      const warnings = host.getResolutionWarningsSnapshot();
      for (const retired of [LEGACY_CUTEBOT_COORDINATE, LEGACY_YAHBOOM_COORDINATE]) {
        assert.ok(
          warnings.some(
            (warning) =>
              warning.kind === "catalog-move-failed" &&
              warning.code === CatalogMoveWarningCode.FETCH_FAILED &&
              warning.origin === retired
          ),
          `the outage load records the move-fetch failure for ${retired}: ${JSON.stringify(warnings)}`
        );
      }
      // The indicator model derives exactly the failed libraries' coordinates.
      assert.deepEqual(
        unresolvedLibraryCoordinates(warnings).sort(),
        [LEGACY_CUTEBOT_COORDINATE, LEGACY_YAHBOOM_COORDINATE].sort()
      );

      let notified = 0;
      host.subscribeToResolutionWarnings(() => {
        notified += 1;
      });
      restore();
      await host.switchProject(defaultProjectId);
      await host.switchProject(importedProjectId);
      assert.ok(notified > 0, "the healed load notified the warning subscriber");
      assert.deepEqual(
        unresolvedLibraryCoordinates(host.getResolutionWarningsSnapshot()),
        [],
        "no library remains unresolved after the heal"
      );
    } finally {
      host.dispose();
    }
  });

  // The export's platform tile refs serialize as plain host-action tile ids
  // whose keys are stable; they resolve against the module catalog on load.
  test("the export's platform radio tile refs resolve on import", async () => {
    const { host } = await importCuterbotProject();
    try {
      const remote = host.getCachedBrain(REMOTE_BRAIN_KEY);
      const drive = host.getCachedBrain(DRIVE_BRAIN_KEY);
      assert.ok(remote, "the imported remote brain deserialized into the cache");
      assert.ok(drive, "the imported drive brain deserialized into the cache");
      assert.deepEqual(kindsOfTile(remote, RADIO_SEND_TILE_ID), ["actuator"]);
      assert.deepEqual(kindsOfTile(drive, RADIO_RECEIVE_BUFFER_TILE_ID), ["sensor"]);
    } finally {
      host.dispose();
    }
  });
});
