import "fake-indexeddb/auto";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { registerHooks } from "node:module";
import { describe, test } from "node:test";
import { fileURLToPath } from "node:url";
import type { ExtensionFetchTransport } from "@wendoo/app-host";
import { createIdbProjectStore, DEFAULT_PROJECT_NAME, ProjectManager } from "@wendoo/app-host";
import type { EmbeddedExtension, UserTileApplyResult } from "@wendoo/bridge-app";
import { AppEnvironmentHost } from "@wendoo/bridge-app";
import { buildEmbeddedExtensionFromDir } from "@wendoo/bridge-app/node";
import { type BrainDef, coreModule, mkVariableFactoryTileId } from "@wendoo/core/app";
import type { IBrainDef } from "@wendoo/core/brain";
import { TypeDiagCode } from "@wendoo/core/brain/compiler";
import { BrainRuleDef } from "@wendoo/core/brain/model";
import type { BrainTileFactoryDef } from "@wendoo/core/brain/tiles";
import { createProfileNumerics } from "@wendoo/core/runtime";
import { isCompilerControlledPath, type WorkspaceCompileResult } from "@wendoo/ts-compiler";
import {
  createWodalSharedModule,
  getWodalDeviceProfile,
  type WodalDeviceProfile,
  WodalDeviceProfileId,
} from "@wendoo/wodal";
import {
  buildMicrobitExtensionEntries,
  installMicrobitReference,
  microbitLibraryCatalogMoves,
  uninstallMicrobitExtension,
} from "../services/microbit-extension-browser";
import {
  CODAL_LIB_COORDINATE,
  CORE_LIB_COORDINATE,
  CUTEBOT_EXT_COORDINATE,
  MICROBIT_V2_LIB_COORDINATE,
  MICROBIT_V2_TARGET_COORDINATE,
  microbitDefaultExtensions,
  YAHBOOM_GAMEPAD_EXT_COORDINATE,
} from "../services/microbit-extension-coordinates";
import type { MicrobitSimEnvironmentStore } from "../services/microbit-sim-environment-store";
import { SIMULATOR_STATE_KEY } from "../services/project-io";
import { POSITION_IDENTITY } from "./extension-test-harness";
import {
  CUTEBOT_GH_REF,
  createPublishedLibraryFixtureTransport,
  YAHBOOM_GAMEPAD_GH_REF,
} from "./published-library-fixtures";

// The store module imports the Vite-provided `virtual:wendoo-embedded-extensions`
// bundle module, which Node's loader cannot resolve. The tests never call
// `MicrobitSimEnvironmentStore.create()` (the only consumer of that bundle);
// map the virtual specifier to an empty stub bundle and load the store module
// dynamically after the hook is registered.
const VIRTUAL_EMBEDDED_EXTENSIONS = "virtual:wendoo-embedded-extensions";
registerHooks({
  resolve(specifier, context, nextResolve) {
    if (specifier === VIRTUAL_EMBEDDED_EXTENSIONS) {
      return { url: new URL("./embedded-extensions-stub.mjs", import.meta.url).href, shortCircuit: true };
    }
    return nextResolve(specifier, context);
  },
});
const storeModule = await import("../services/microbit-sim-environment-store");
const StoreClass = storeModule.MicrobitSimEnvironmentStore;

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

/** The app's full embed record, assembled exactly as `vite/embedded-extensions.mjs` registers it. */
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

/** The Cutebot steer actuator's runtime tile id: its stable action id under the published coordinate. */
const STEER_TILE_ID = `tile.actuator->${CUTEBOT_EXT_COORDINATE}:user.actuator.mwQw6UokYjp3HVBD`;

/** Wait long enough for the user-code reflasher's 250ms debounce and any fire-and-forget persistence to settle. */
function settle(ms = 400): Promise<void> {
  return new Promise((resolve) => {
    setTimeout(resolve, ms);
  });
}

/**
 * Manufacture a Position brain variable from the installed Position library's
 * variable-factory tile and register it on the brain's own catalog.
 */
function makePositionVariable(
  store: MicrobitSimEnvironmentStore,
  brainDef: IBrainDef,
  lastCompile: () => WorkspaceCompileResult | undefined
) {
  const positionTypeId = store.env.brainServices.runtime.types.resolveByName(POSITION_IDENTITY);
  assert.ok(positionTypeId, "the Position struct type is registered");
  const factory = (lastCompile()?.bundle?.tiles ?? []).find(
    (tile) => tile.tileId === mkVariableFactoryTileId(positionTypeId)
  ) as BrainTileFactoryDef | undefined;
  assert.ok(factory, "the Position variable factory is in the bundle");
  const variable = factory.manufacture(factory, { name: "held" });
  assert.ok(variable, "the factory manufactured a position variable tile");
  brainDef.catalog().registerTileDef(variable);
  return variable;
}

/** The persisted simulator fleet record: ordered instance ids and the brain each instance is flashed with. */
interface PersistedFleet {
  readonly order: readonly string[];
  readonly flash: Readonly<Record<string, string>>;
}

/** Parse the durable simulator-state app-data record of the active project. */
async function loadPersistedFleet(projectManager: ProjectManager): Promise<PersistedFleet> {
  const raw = await projectManager.loadAppData(SIMULATOR_STATE_KEY);
  assert.ok(raw, "the project persisted a simulator-state record");
  return JSON.parse(raw) as PersistedFleet;
}

let storeCounter = 0;

/**
 * The store exactly as the browser app runs it, with the headless
 * substitutions stated where they occur: the IDB store runs over
 * fake-indexeddb, the jsDelivr transport is replaced by the published-library
 * fetch fixture (no network in tests), and the Web Locks project lock and the
 * bridge URL are omitted (browser-only surfaces this flow never consumes).
 * The host is constructed as `MicrobitSimEnvironmentStore.create()` constructs
 * it, including the `onDidCompile` wiring into the store's user-code
 * reflasher, and the store is the real `MicrobitSimEnvironmentStore` over that
 * host.
 */
async function makeProductionShapedStore(
  transport: ExtensionFetchTransport,
  storeName = `library-uninstall-reinstall-${storeCounter++}`
): Promise<{
  store: MicrobitSimEnvironmentStore;
  host: AppEnvironmentHost;
  embedRecord: EmbeddedExtension[];
  storeName: string;
  lastCompile: () => WorkspaceCompileResult | undefined;
}> {
  const activeProfile = getWodalDeviceProfile(WodalDeviceProfileId.MICROBIT_V2);
  const projectStore = await createIdbProjectStore(storeName);
  const embedRecord = appEmbedRecord();
  let lastCompile: WorkspaceCompileResult | undefined;
  let storeRef: MicrobitSimEnvironmentStore | undefined;
  const host = new AppEnvironmentHost({
    projectManager: new ProjectManager(projectStore, {
      filesystemOptions: {
        shouldExclude: (path) => isCompilerControlledPath(path, []),
      },
      defaultExtensions: microbitDefaultExtensions,
    }),
    modules: [coreModule(), createWodalSharedModule(), activeProfile.createWendooModule()],
    numerics: createProfileNumerics(activeProfile.numberPrecision),
    mounts: [],
    embeddedExtensions: embedRecord,
    extensionFetchTransport: transport,
    catalogMoves: microbitLibraryCatalogMoves,
    rng: { next: () => Math.random() },
    onDidCompile: (result, tileResult) => {
      lastCompile = result;
      // The same wiring `MicrobitSimEnvironmentStore.create()` installs: each
      // compile feeds the store's user-code reflasher.
      (
        storeRef as unknown as { handleProjectCompiled(tileResult: UserTileApplyResult | undefined): void } | undefined
      )?.handleProjectCompiled(tileResult);
    },
  });
  // The store's constructor is TypeScript-private (production creation goes
  // through `create()`, which hardcodes the jsDelivr transport); the runtime
  // constructor is invoked directly so the real store logic runs over the
  // fixture-transport host.
  type StoreCtor = new (
    host: AppEnvironmentHost,
    activeDeviceProfile: WodalDeviceProfile,
    folderSession: undefined
  ) => MicrobitSimEnvironmentStore;
  const store = new (StoreClass as unknown as StoreCtor)(host, activeProfile, undefined);
  storeRef = store;
  return { store, host, embedRecord, storeName, lastCompile: () => lastCompile };
}

/**
 * Seed the user's observed starting state through the same store and host
 * actions the browser UI calls, across an app restart so the cached brains
 * are the deserialized instances a live session holds. Session one
 * initializes onto the default project, installs Cutebot from the
 * library-browser offer, saves a brain whose rule is DO [cutebot steer,
 * position variable] (the filled anonymous slot references the Position
 * library's anonymous parameter tile -- the user-observed brain shape), and
 * flashes that brain onto the simulator instance. Session two reopens the
 * same durable store as an app reload: the brain deserializes into the cache
 * and the fleet restore re-flashes it from the persisted mapping.
 */
async function seedRestartedFlashedProject(): Promise<{
  store: MicrobitSimEnvironmentStore;
  host: AppEnvironmentHost;
  embedRecord: EmbeddedExtension[];
  brainId: string;
  untouchedBrainId: string;
  bystanderBrainId: string;
  instanceId: string;
}> {
  const first = await makeProductionShapedStore(createPublishedLibraryFixtureTransport());
  await first.store.initialize();
  // The project-loaded listener restores the fleet fire-and-forget; let it settle.
  await settle(100);

  const install = await installMicrobitReference(
    first.host,
    first.store.activeProjectManifest?.extensions,
    first.embedRecord,
    CUTEBOT_GH_REF
  );
  assert.ok(install.ok, "the offer install input normalizes");
  assert.ok(install.action.ok, "the offer install adds the reference");
  assert.equal(install.report?.committed, true, "the offer install transaction commits");

  const steer = (first.lastCompile()?.bundle?.tiles ?? []).find((tile) => tile.tileId === STEER_TILE_ID);
  assert.ok(steer, "the Cutebot steer tile registered from the installed library");

  const brainId = await first.store.addBrain("robot drive");
  // The cache holds the BrainDef the store created; the same narrowing the store's own accessors use.
  const brainDef = first.store.host.getCachedBrain(brainId) as BrainDef | undefined;
  assert.ok(brainDef, "the new brain is cached");
  const doClause = brainDef.pages().get(0)!.children().get(0)!.do();
  doClause.appendTile(steer);
  doClause.appendTile(makePositionVariable(first.store, brainDef, first.lastCompile));
  await first.store.saveBrain(brainId, brainDef);

  // A second Cutebot brain that is never flashed and never touched again: it
  // sits in the brain list from project load on.
  const untouchedBrainId = await first.store.addBrain("robot drive parked");
  const untouchedDef = first.store.host.getCachedBrain(untouchedBrainId) as BrainDef | undefined;
  assert.ok(untouchedDef, "the untouched brain is cached");
  const untouchedDo = untouchedDef.pages().get(0)!.children().get(0)!.do();
  untouchedDo.appendTile(steer);
  untouchedDo.appendTile(makePositionVariable(first.store, untouchedDef, first.lastCompile));
  await first.store.saveBrain(untouchedBrainId, untouchedDef);

  // A bystander brain using no Cutebot tile at all.
  const bystanderBrainId = await first.store.addBrain("bystander");

  const seededInstance = first.store.simulator.getInstances()[0];
  assert.ok(seededInstance, "the restored fleet holds one instance");
  await first.store.flashBrainToInstance(seededInstance.id, brainId);
  assert.equal(seededInstance.flashState.status, "loaded", "the seeded flash loads");
  const instanceId = seededInstance.id;
  await settle(100);
  const seededFleet = await loadPersistedFleet(first.store.projectManager);
  assert.equal(seededFleet.flash[instanceId], brainId, "the seeded flash mapping persisted");
  first.store.dispose();

  // The app restart: a fresh store over the same durable store.
  const second = await makeProductionShapedStore(createPublishedLibraryFixtureTransport(), first.storeName);
  await second.store.initialize();
  await settle(200);
  const restored = second.store.simulator.getInstances().find((candidate) => candidate.id === instanceId);
  assert.ok(restored, "the reload restored the persisted instance");
  assert.equal(restored.flashState.status, "loaded", "the reload re-flashed the persisted brain");
  assert.equal(restored.flashedBrainId, brainId, "the restored instance runs the seeded brain");
  return {
    store: second.store,
    host: second.host,
    embedRecord: second.embedRecord,
    brainId,
    untouchedBrainId,
    bystanderBrainId,
    instanceId,
  };
}

/**
 * The error diagnostics carried by the STORED per-rule TypecheckResults of a
 * cached brain, read directly off its tilesets. Informational conversion
 * diagnostics are excluded, matching the badge derivation.
 */
function storedDiagnosticCount(store: MicrobitSimEnvironmentStore, brainId: string): number {
  const brainDef = store.host.getCachedBrain(brainId);
  assert.ok(brainDef, `brain ${brainId} is cached`);
  let count = 0;
  brainDef.pages().forEach((page) => {
    page.children().forEach((rule) => {
      if (!(rule instanceof BrainRuleDef)) {
        return;
      }
      const result = rule.when().typecheckResult();
      if (!result) {
        return;
      }
      count += result.whenParseResult.diags.size() + result.doParseResult.diags.size();
      result.typeInfo.diags.forEach((diag) => {
        if (diag.code !== TypeDiagCode.DataTypeConverted) {
          count += 1;
        }
      });
    });
  });
  return count;
}

/** Uninstall Cutebot through the library-browser card's action path. */
async function uninstallCutebot(store: MicrobitSimEnvironmentStore, embedRecord: EmbeddedExtension[]) {
  const result = await uninstallMicrobitExtension(
    store.host,
    store.activeProjectManifest?.extensions,
    CUTEBOT_EXT_COORDINATE,
    embedRecord,
    store.host.installedExtensionContent
  );
  assert.ok(result.action.ok, "the uninstall removes the reference");
  assert.equal(result.report?.committed, true, "the uninstall transaction commits");
  return result;
}

describe("library uninstall -> offer reinstall -- production store wiring", () => {
  // The browser panel derives its entry cards inside the active-project
  // notification: `ProjectHeader` recomputes `buildMicrobitExtensionEntries`
  // from the manifest extensions (via `subscribeToActiveProject`) and the
  // host's live `installedExtensionContent` / `extensionFetchFailures`. The
  // reinstall must never present the just-installed reference as broken at any
  // of those notifications: the transaction swaps the fetched snapshots into
  // `installedExtensionContent` before the manifest commit fires the
  // notification, so every notification-time derivation sees the installed
  // content.
  test("an offer reinstall after uninstall is never derivable as broken at an active-project notification", async () => {
    const { store, host, embedRecord } = await seedRestartedFlashedProject();
    // The exact derivation the browser panel performs on each active-project
    // notification, recorded for every notification `action` causes.
    async function brokenStatesDuring(action: () => Promise<void>): Promise<(object | undefined)[]> {
      const observed: (object | undefined)[] = [];
      const unsubscribe = host.projectManager.onActiveProjectChange(() => {
        const entries = buildMicrobitExtensionEntries(
          host.activeProjectManifest?.extensions,
          embedRecord,
          host.installedExtensionContent,
          host.extensionFetchFailures
        );
        const cutebot = entries.find((entry) => entry.coordinate === CUTEBOT_EXT_COORDINATE);
        if (cutebot !== undefined) {
          observed.push(cutebot.broken);
        }
      });
      try {
        await action();
      } finally {
        unsubscribe();
      }
      return observed;
    }
    try {
      await uninstallCutebot(store, embedRecord);
      await settle();

      const observedBrokenStates = await brokenStatesDuring(async () => {
        const reinstall = await installMicrobitReference(
          store.host,
          store.activeProjectManifest?.extensions,
          embedRecord,
          CUTEBOT_GH_REF
        );
        assert.ok(reinstall.ok, "the offer reinstall input normalizes");
        assert.ok(reinstall.action.ok, "the offer reinstall adds the reference");
        assert.equal(reinstall.report?.committed, true, "the offer reinstall transaction commits");
      });
      assert.ok(observedBrokenStates.length > 0, "the reinstall commit notified active-project subscribers");

      // After the transaction resolves, the same derivation is healthy; a
      // broken observation above is therefore what the panel keeps rendering.
      const settled = buildMicrobitExtensionEntries(
        host.activeProjectManifest?.extensions,
        embedRecord,
        host.installedExtensionContent,
        host.extensionFetchFailures
      ).find((entry) => entry.coordinate === CUTEBOT_EXT_COORDINATE);
      assert.ok(settled, "the reinstalled library has an entry card");
      assert.equal(settled.broken, undefined, "the settled entry card is not broken");

      // The user-observed recovery: the panel's Retry re-runs the transaction
      // over the same extensions map, and at ITS commit notification the
      // installed content is already present, so the derivation is healthy.
      const retryBrokenStates = await brokenStatesDuring(async () => {
        const retry = await host.updateProjectExtensions(host.activeProjectManifest?.extensions ?? {});
        assert.equal(retry.committed, true, "the retry transaction commits");
      });
      for (const broken of retryBrokenStates) {
        assert.equal(broken, undefined, "the retry's notifications derive a healthy entry");
      }

      for (const broken of observedBrokenStates) {
        assert.equal(broken, undefined, "a notification during the reinstall derived the installed library as broken");
      }
    } finally {
      store.dispose();
    }
  });

  // The brain record survives the whole flow, and the user ruling holds: the
  // simulator flash mapping must never reset while the brain still exists.
  // The uninstall's recompile makes the reflash fail, but the failed instance
  // keeps its brain association (in memory and in the persisted fleet
  // snapshot), and the reinstall's recompile re-flashes the associated
  // instance back to the loaded state -- the heal needs no user action.
  test("the flash mapping survives uninstall -> reinstall -> recompile while the brain record persists", async () => {
    const { store, embedRecord, brainId, instanceId } = await seedRestartedFlashedProject();
    try {
      assert.deepEqual(store.getBrainDiagnostics(brainId), [], "the healthy brain carries no error-badge state");
      const revisionBeforeUninstall = store.getBrainDiagnosticsRevision();
      const uninstall = await uninstallCutebot(store, embedRecord);
      // The uninstall is the transaction that degrades the brain: its report
      // carries the brain-located new problems the toast surfaces.
      assert.equal(uninstall.report?.committed, true);
      assert.equal(uninstall.report?.outcome.kind, "worsened", "removing an in-use library worsens the outcome");
      assert.ok(
        uninstall.report?.outcome.newProblems.some((problem) => problem.location === brainId),
        "the worsened outcome locates a new problem at the flashed brain"
      );

      // The brain badge state: the degraded brain carries its stored
      // typecheck diagnostics verbatim, and the transaction signals the badge
      // subscription.
      assert.ok(
        store.getBrainDiagnosticsRevision() > revisionBeforeUninstall,
        "the uninstall transaction bumps the brain-diagnostics revision"
      );
      const degraded = store.getBrainDiagnostics(brainId);
      assert.ok(degraded.length > 0, "the degraded brain carries stored error diagnostics");
      for (const diagnostic of degraded) {
        assert.equal(typeof diagnostic.code, "number");
        assert.ok(diagnostic.message.length > 0, "each row carries the compiler's message");
      }
      await settle();

      const reinstall = await installMicrobitReference(
        store.host,
        store.activeProjectManifest?.extensions,
        embedRecord,
        CUTEBOT_GH_REF
      );
      assert.ok(reinstall.ok && reinstall.action.ok, "the offer reinstall runs");
      assert.equal(reinstall.report?.committed, true, "the offer reinstall transaction commits");
      await settle();

      // The heal: the reinstall's recompile re-flashed the associated instance
      // back to the loaded state with the same brain, with no user action.
      const healed = store.simulator.getInstances().find((candidate) => candidate.id === instanceId);
      assert.ok(healed, "the instance persists through the flow");
      assert.equal(healed.flashState.status, "loaded", "the reinstall's recompile re-flashes the instance");
      assert.equal(healed.flashedBrainId, brainId, "the healed instance runs the same brain");
      assert.deepEqual(store.getBrainDiagnostics(brainId), [], "the reinstall clears the brain's error-badge state");

      // The brain record persisted through the whole flow.
      const brainsRaw = await store.projectManager.loadAppData("brains");
      assert.ok(brainsRaw, "the project still persists a brains record");
      const brains = JSON.parse(brainsRaw) as Record<string, unknown>;
      assert.ok(brainId in brains, "the brain record persists through uninstall and reinstall");
      assert.ok(store.host.getCachedBrain(brainId), "the brain stays in the live cache");

      const fleet = await loadPersistedFleet(store.projectManager);
      assert.equal(
        fleet.flash[instanceId],
        brainId,
        "the flash mapping app-data entry must survive while the brain exists"
      );
    } finally {
      store.dispose();
    }
  });

  // The badge model's per-brain consistency contract, both directions, with
  // NO reload: an extension transaction leaves EVERY cached brain's stored
  // TypecheckResult consistent with the post-transaction bundle before the
  // brain-diagnostics revision signals the UI. The brain states differ on
  // purpose: one brain is flashed (the reflasher also touches it), one just
  // sits in the list untouched since project load, and one uses no Cutebot
  // tile at all.
  test("uninstall marks and reinstall heals the stored typecheck state of every cached brain without a reload", async () => {
    const { store, embedRecord, brainId, untouchedBrainId, bystanderBrainId } = await seedRestartedFlashedProject();
    try {
      for (const id of [brainId, untouchedBrainId, bystanderBrainId]) {
        assert.deepEqual(store.getBrainDiagnostics(id), [], `brain ${id} starts clean`);
      }

      // Direction 1: uninstall. Assert immediately after the transaction
      // resolves -- no settle, no reload -- for each brain state.
      await uninstallCutebot(store, embedRecord);
      assert.ok(storedDiagnosticCount(store, brainId) > 0, "the flashed brain's stored results reflect the removal");
      assert.ok(
        storedDiagnosticCount(store, untouchedBrainId) > 0,
        "the untouched brain's stored results reflect the removal with no reload"
      );
      assert.ok(store.getBrainDiagnostics(brainId).length > 0, "the flashed brain shows badge rows");
      assert.ok(store.getBrainDiagnostics(untouchedBrainId).length > 0, "the untouched brain shows badge rows");
      assert.deepEqual(store.getBrainDiagnostics(bystanderBrainId), [], "the bystander brain gains no badge");
      await settle();

      // Direction 2: reinstall. Every affected brain heals -- again asserted
      // immediately after the transaction, before any reflash settles.
      const reinstall = await installMicrobitReference(
        store.host,
        store.activeProjectManifest?.extensions,
        embedRecord,
        CUTEBOT_GH_REF
      );
      assert.ok(reinstall.ok && reinstall.action.ok, "the offer reinstall runs");
      assert.equal(reinstall.report?.committed, true, "the offer reinstall transaction commits");
      assert.equal(storedDiagnosticCount(store, brainId), 0, "the flashed brain's stored results heal");
      assert.equal(
        storedDiagnosticCount(store, untouchedBrainId),
        0,
        "the untouched brain's stored results heal with no reload"
      );
      assert.deepEqual(store.getBrainDiagnostics(brainId), [], "the flashed brain's badge clears");
      assert.deepEqual(store.getBrainDiagnostics(untouchedBrainId), [], "the untouched brain's badge clears");
      assert.deepEqual(store.getBrainDiagnostics(bystanderBrainId), [], "the bystander brain stays clean");
      await settle();
    } finally {
      store.dispose();
    }
  });

  // The user-reported artifact, replayed exactly: an imported two-brain
  // project (both brains flashed to two simulators, plus a host user-tile file
  // carrying its own compile error), Yahboom uninstalled, reloaded,
  // reinstalled, reloaded. The badge contract at every step: an extension
  // transaction leaves each cached brain's stored typecheck state IDENTICAL to
  // what a fresh load of the same persisted project produces -- transaction
  // time and load time never disagree, in either direction, and no step needs
  // a reload to show or clear badges.
  test("the imported user project's badges track uninstall and reinstall exactly like a reload would", async () => {
    const documentText = readFileSync(
      fileURLToPath(new URL("../../test-fixtures/cuterbot-user-report.wendoo", import.meta.url)),
      "utf8"
    );
    let live: MicrobitSimEnvironmentStore | undefined;
    try {
      const first = await makeProductionShapedStore(createPublishedLibraryFixtureTransport());
      live = first.store;
      await first.store.initialize();
      await settle(200);

      const imported = await first.store.importProject(new File([documentText], "cuterbot (1).wendoo"));
      assert.equal(imported.success, true, "the user document imports");
      await settle(300);
      const brainIds = first.store.getBrains().map((brain) => brain.id);
      assert.equal(brainIds.length, 2, "both brains import");
      for (const id of brainIds) {
        assert.deepEqual(first.store.getBrainDiagnostics(id), [], `brain ${id} imports clean`);
      }

      // Uninstall Yahboom. BOTH brains use its tiles; both are flagged
      // immediately, with no reload.
      const uninstall = await uninstallMicrobitExtension(
        first.store.host,
        first.store.activeProjectManifest?.extensions,
        YAHBOOM_GAMEPAD_EXT_COORDINATE,
        first.embedRecord,
        first.store.host.installedExtensionContent
      );
      assert.ok(uninstall.action.ok, "the uninstall removes the reference");
      assert.equal(uninstall.report?.committed, true, "the uninstall transaction commits");
      const transactionRows = new Map(brainIds.map((id) => [id, first.store.getBrainDiagnostics(id)]));
      for (const id of brainIds) {
        assert.ok(storedDiagnosticCount(first.store, id) > 0, `brain ${id} stored results reflect the removal`);
        assert.ok((transactionRows.get(id)?.length ?? 0) > 0, `brain ${id} is flagged immediately after uninstall`);
      }
      await settle();
      first.store.dispose();
      live = undefined;

      // Reload. The load-time state is the truth the transaction-time state
      // must already have matched, row for row.
      const second = await makeProductionShapedStore(createPublishedLibraryFixtureTransport(), first.storeName);
      live = second.store;
      await second.store.initialize();
      await settle(300);
      for (const id of brainIds) {
        assert.deepEqual(
          transactionRows.get(id),
          second.store.getBrainDiagnostics(id),
          `brain ${id} transaction-time rows equal load-time rows`
        );
      }

      // Reinstall Yahboom. Both brains heal immediately, with no reload.
      const reinstall = await installMicrobitReference(
        second.store.host,
        second.store.activeProjectManifest?.extensions,
        second.embedRecord,
        YAHBOOM_GAMEPAD_GH_REF
      );
      assert.ok(reinstall.ok && reinstall.action.ok, "the reinstall runs");
      assert.equal(reinstall.report?.committed, true, "the reinstall transaction commits");
      for (const id of brainIds) {
        assert.equal(storedDiagnosticCount(second.store, id), 0, `brain ${id} stored results heal on reinstall`);
        assert.deepEqual(second.store.getBrainDiagnostics(id), [], `brain ${id} badge clears with no reload`);
      }
      await settle();
      second.store.dispose();
      live = undefined;

      // Reload again: still clean.
      const third = await makeProductionShapedStore(createPublishedLibraryFixtureTransport(), first.storeName);
      live = third.store;
      await third.store.initialize();
      await settle(300);
      for (const id of brainIds) {
        assert.deepEqual(third.store.getBrainDiagnostics(id), [], `brain ${id} stays clean across the final reload`);
      }
    } finally {
      live?.dispose();
    }
  });
});
