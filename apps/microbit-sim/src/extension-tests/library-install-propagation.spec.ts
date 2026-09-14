/**
 * Pins the install -> tile-picker/docs propagation path for the microbit-sim
 * app as the app itself wires it: a project seeded with the micro:bit v2 target
 * (which pulls the standard-library layer in transitively), driven through the
 * real project manager and the browser's catalog-offer install action, with the
 * offers' published `gh:` content served by the fixture transport. It asserts
 * that after installing the Cutebot and Yahboom libraries, their compiled tiles
 * surface in the exact data the brain tile picker reads (the environment tile
 * catalogs, via `suggestTiles`) and in the data the docs panel reads (the docs
 * registry built from the host's user-tile metadata), and that both libraries
 * are reported as installed. This is the app path (target-seeded) that the
 * extension-test harness -- which seeds the standard-library layer directly --
 * does not cover.
 */

import assert from "node:assert/strict";
import { describe, it } from "node:test";
import { fileURLToPath } from "node:url";
import {
  type ActiveProject,
  createInMemoryProjectFileSystem,
  type ProjectFileSystem,
  type ProjectManager,
} from "@wendoo/app-host";
import { AppEnvironmentHost, type EmbeddedExtension } from "@wendoo/bridge-app";
import { buildEmbeddedExtensionFromDir } from "@wendoo/bridge-app/node";
import { List } from "@wendoo/core";
import { coreModule, mkActuatorTileId, mkSensorTileId } from "@wendoo/core/app";
import { type ITileCatalog, RuleSide } from "@wendoo/core/brain";
import { type InsertionContext, suggestTiles } from "@wendoo/core/brain/language-service";
import { createProfileNumerics } from "@wendoo/core/runtime";
import { createWodalSharedModule, getWodalDeviceProfile, WodalDeviceProfileId } from "@wendoo/wodal";
import { buildMicrobitBrainEditorConfig } from "../brain/editor-config";
import { createMicrobitDocsRegistry } from "../docs/docs-registry";
import {
  buildMicrobitCatalogOffers,
  installMicrobitReference,
  microbitLibraryCatalogMoves,
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
import { createPublishedLibraryFixtureTransport } from "./published-library-fixtures";

function extensionDir(relativePath: string): string {
  return fileURLToPath(new URL(relativePath, import.meta.url));
}

/** The embed record the Vite provider assembles: the target and the three layers; the feature libraries are catalog offers fetched as published gh: content. */
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

/**
 * A minimal project manager over an in-memory file system, seeded exactly as a
 * new microbit-sim project (the target coordinate). `updateActive` merges the
 * applied extensions map, mirroring the install transaction's manifest write.
 */
function seededProjectManager(filesystem: ProjectFileSystem): ProjectManager {
  const appData = new Map<string, string>();
  let activeProject: ActiveProject = {
    manifest: {
      id: "p1",
      projectCollectionId: "c1",
      name: "p1",
      version: "0.1.0",
      description: "",
      createdAt: 1,
      updatedAt: 1,
      extensions: { ...microbitDefaultExtensions },
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

/** Builds the app's host over the seeded in-memory project, wired exactly as the microbit-sim store wires it. */
function makeHost(): AppEnvironmentHost {
  const profile = getWodalDeviceProfile(WodalDeviceProfileId.MICROBIT_V2);
  return new AppEnvironmentHost({
    projectManager: seededProjectManager(createInMemoryProjectFileSystem()),
    modules: [coreModule(), createWodalSharedModule(), profile.createWendooModule()],
    numerics: createProfileNumerics(profile.numberPrecision),
    mounts: [],
    embeddedExtensions: microbitEmbedRecord(),
    catalogMoves: microbitLibraryCatalogMoves,
    extensionFetchTransport: createPublishedLibraryFixtureTransport(),
  });
}

/** The compatibility-filtered catalog offer for `coordinate`, as the browser lists it for the host's project. */
function offerFor(host: AppEnvironmentHost, coordinate: string) {
  const offers = buildMicrobitCatalogOffers(host.activeProjectManifest?.extensions, microbitEmbedRecord());
  const offer = offers.find((candidate) => candidate.coordinate === coordinate);
  assert.ok(offer, `the catalog offers ${coordinate} for this project`);
  return offer;
}

/** The tile ids the brain tile picker offers for the given rule side, via the app's editor config and `suggestTiles`. */
function pickerTileIds(host: AppEnvironmentHost, side: RuleSide): string[] {
  const config = buildMicrobitBrainEditorConfig({
    env: host.env,
    resolveVfsAssetUrl: (url) => url,
    projectNamespace: host.activeProjectManifest?.id,
    libraries: host.installedLibraries,
  });
  const context: InsertionContext = { ruleSide: side };
  const catalogs = List.from<ITileCatalog>(config.tileCatalogs ?? []).asReadonly();
  assert.ok(config.brainServices, "the editor config carries brain services");
  const result = suggestTiles(context, catalogs, config.brainServices);
  const ids: string[] = [];
  for (let i = 0; i < result.exact.size(); i++) {
    ids.push(result.exact.get(i)!.tileDef.tileId);
  }
  return ids;
}

describe("library install propagation (app path)", () => {
  it("cutebot and yahboom tiles reach the tile picker and docs panel after a browser install", async () => {
    const restore = installLocalStorage();
    const host = makeHost();
    try {
      await host.initialize("p1");

      // Baseline: the add-ons are not installed, so their tiles are absent from the picker.
      const cutebotDrive = mkActuatorTileId(`${CUTEBOT_EXT_COORDINATE}:user.actuator.zClgsu5drwdekq7r`);
      assert.equal(
        pickerTileIds(host, RuleSide.Do).includes(cutebotDrive),
        false,
        "the cutebot drive tile is absent before install"
      );

      // Install each catalog offer through the exact action ProjectHeader
      // drives for an offer card: installMicrobitReference with the offer's
      // pinned gh: reference.
      const cutebotOutcome = await installMicrobitReference(
        host,
        host.activeProjectManifest?.extensions,
        microbitEmbedRecord(),
        offerFor(host, CUTEBOT_EXT_COORDINATE).ref
      );
      assert.equal(cutebotOutcome.ok, true, "the cutebot offer input normalizes");
      assert.ok(cutebotOutcome.ok && cutebotOutcome.action.ok, "the cutebot install action succeeds");
      assert.equal(
        cutebotOutcome.ok ? cutebotOutcome.report?.committed : undefined,
        true,
        "the cutebot install transaction commits"
      );

      const yahboomOutcome = await installMicrobitReference(
        host,
        host.activeProjectManifest?.extensions,
        microbitEmbedRecord(),
        offerFor(host, YAHBOOM_GAMEPAD_EXT_COORDINATE).ref
      );
      assert.equal(yahboomOutcome.ok, true, "the yahboom offer input normalizes");
      assert.ok(yahboomOutcome.ok && yahboomOutcome.action.ok, "the yahboom install action succeeds");
      assert.equal(
        yahboomOutcome.ok ? yahboomOutcome.report?.committed : undefined,
        true,
        "the yahboom install transaction commits"
      );

      // The picker's data source now offers the installed libraries' tiles.
      const actuatorTiles = pickerTileIds(host, RuleSide.Do);
      assert.equal(actuatorTiles.includes(cutebotDrive), true, "the cutebot drive actuator tile reaches the picker");
      assert.ok(
        actuatorTiles.some((id) => id.includes(`${CUTEBOT_EXT_COORDINATE}:user.actuator.`)),
        "cutebot actuator tiles reach the picker"
      );

      const whenTiles = pickerTileIds(host, RuleSide.When);
      assert.ok(
        whenTiles.some((id) => id.includes(`${YAHBOOM_GAMEPAD_EXT_COORDINATE}:user.sensor.`)),
        "yahboom sensor tiles reach the picker"
      );

      // The host reports both libraries as installed (the docs panel and picker library grouping read this).
      const installedCoordinates = host.installedLibraries.map((library) => library.coordinate);
      assert.ok(installedCoordinates.includes(CUTEBOT_EXT_COORDINATE), "cutebot is reported installed");
      assert.ok(installedCoordinates.includes(YAHBOOM_GAMEPAD_EXT_COORDINATE), "yahboom is reported installed");

      // The docs registry the docs panel reads carries an entry for every installed-library tile,
      // and the extension's authored markdown propagates into the entry content.
      const registry = createMicrobitDocsRegistry(host.env, host.lastUserTileMetadata);
      assert.equal(registry.tiles.has(cutebotDrive), true, "the cutebot drive tile has a docs entry");
      const driveEntry = registry.tiles.get(cutebotDrive);
      assert.ok(driveEntry && driveEntry.content.length > 0, "the cutebot drive doc entry carries authored content");

      const yahboomButton = host.lastUserTileMetadata?.find(
        (m) => m.namespace === YAHBOOM_GAMEPAD_EXT_COORDINATE && m.kind === "sensor"
      );
      assert.ok(yahboomButton, "yahboom compiled a sensor tile");
      const yahboomTileId = mkSensorTileId(yahboomButton.key);
      assert.equal(registry.tiles.has(yahboomTileId), true, "a yahboom sensor tile has a docs entry");
    } finally {
      host.dispose();
      restore();
    }
  });
});
