import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { describe, test } from "node:test";
import type { ExtensionCatalogDocumentEntry, ExtensionCatalogMoveEntry } from "@wendoo/app-host";
import { CATALOG_ENTRY_KIND_EXTENSION, validateExtensionCatalogDocument } from "@wendoo/app-host";
import type { EmbeddedExtension, FetchedExtensionContentMap } from "@wendoo/bridge-app";
import { ExtensionActionResultCode } from "@wendoo/bridge-app";
import {
  buildMicrobitCatalogOffers,
  buildMicrobitExtensionEntries,
  buildMicrobitLibraryShelf,
  type ExtensionProjectPersistence,
  loadMicrobitLibraryCatalog,
  uninstallMicrobitExtension,
} from "./microbit-extension-browser";
import {
  CODAL_LIB_COORDINATE,
  CORE_LIB_COORDINATE,
  CUTEBOT_EXT_COORDINATE,
  MICROBIT_V2_LIB_COORDINATE,
  MICROBIT_V2_LIB_REFERENCE,
  MICROBIT_V2_TARGET_COORDINATE,
  MICROBIT_V2_TARGET_REFERENCE,
  YAHBOOM_GAMEPAD_EXT_COORDINATE,
} from "./microbit-extension-coordinates";
import microbitLibraryCatalogDocument from "./microbit-library-catalog.json";

const POSITION = "wendoo-lang/lib-codal-position";

/** The retired embedded coordinates the catalog moves migrate away from. */
const RETIRED_CUTEBOT = "wendoo-lang/lib-microbit-cutebot";
const RETIRED_YAHBOOM = "wendoo-lang/lib-microbit-yahboom-gamepad";

/** The bundled catalog's entry for `coordinate`, as the validated document carries it. */
function catalogEntryFor(coordinate: string): ExtensionCatalogDocumentEntry {
  const result = validateExtensionCatalogDocument(microbitLibraryCatalogDocument);
  assert.ok(result.ok);
  const entry = result.document.entries.find((candidate) => candidate.coordinate === coordinate);
  assert.ok(entry, `the catalog lists ${coordinate}`);
  return entry;
}

/** Read a published library fixture's manifest from its snapshot directory. */
function publishedManifest(dir: string): { name: string; version: string; description: string } {
  const url = new URL(`../../test-fixtures/${dir}/wendoo.json`, import.meta.url);
  return JSON.parse(readFileSync(url, "utf8"));
}

describe("microbit library catalog document", () => {
  test("the seeded catalog validates with no errors and no warnings", () => {
    const result = validateExtensionCatalogDocument(microbitLibraryCatalogDocument);
    assert.ok(result.ok);
    assert.equal(result.errors.length, 0);
    assert.equal(result.warnings.length, 0);
  });

  test("every entry is a library at a full-SHA gh: pin matching its coordinate, with no alias", () => {
    const result = validateExtensionCatalogDocument(microbitLibraryCatalogDocument);
    assert.ok(result.ok);
    // Position is a transitive sub-dependency of the featured chassis libraries;
    // it is redirected by a move, never listed as its own entry.
    assert.deepEqual(
      result.document.entries.map((entry) => entry.coordinate).sort(),
      [CUTEBOT_EXT_COORDINATE, YAHBOOM_GAMEPAD_EXT_COORDINATE].sort()
    );
    for (const entry of result.document.entries) {
      assert.equal(entry.kind, CATALOG_ENTRY_KIND_EXTENSION);
      assert.equal("alias" in entry, false);
      assert.match(entry.ref, new RegExp(`^gh:${entry.coordinate}@[0-9a-f]{40}$`));
    }
  });

  test("each entry's name, version, and description equal its published manifest's", () => {
    const result = validateExtensionCatalogDocument(microbitLibraryCatalogDocument);
    assert.ok(result.ok);
    const byCoordinate = new Map(result.document.entries.map((entry) => [entry.coordinate, entry]));
    for (const [coordinate, dir] of [
      [CUTEBOT_EXT_COORDINATE, "lib-elecfreaks-cutebot"],
      [YAHBOOM_GAMEPAD_EXT_COORDINATE, "lib-yahboom-gamepad"],
    ] as const) {
      const entry = byCoordinate.get(coordinate);
      assert.ok(entry, `the catalog lists ${coordinate}`);
      const manifest = publishedManifest(dir);
      assert.equal(entry.name, manifest.name);
      assert.equal(entry.version, manifest.version);
      assert.equal(entry.description, manifest.description);
    }
  });

  test("each entry lists the exact earlier approved version it accumulated", () => {
    const result = validateExtensionCatalogDocument(microbitLibraryCatalogDocument);
    assert.ok(result.ok);
    const byCoordinate = new Map(result.document.entries.map((entry) => [entry.coordinate, entry]));
    for (const [coordinate, priors] of [
      [
        CUTEBOT_EXT_COORDINATE,
        [
          {
            ref: `gh:${CUTEBOT_EXT_COORDINATE}@16d9d4b39ff257168e262db30bb91d87cfc2042d`,
            version: "0.2.2",
          },
        ],
      ],
      [
        YAHBOOM_GAMEPAD_EXT_COORDINATE,
        [
          {
            ref: `gh:${YAHBOOM_GAMEPAD_EXT_COORDINATE}@4bb75c9f49b8c6f7f9d71b7493fc290cbb344610`,
            version: "0.2.0",
          },
        ],
      ],
    ] as const) {
      const entry = byCoordinate.get(coordinate);
      assert.ok(entry, `the catalog lists ${coordinate}`);
      assert.deepEqual(entry.priors, priors);
    }
  });

  test("each entry's compatibility targets are curator-authored against the stdlib layer", () => {
    const result = validateExtensionCatalogDocument(microbitLibraryCatalogDocument);
    assert.ok(result.ok);
    for (const entry of result.document.entries) {
      // Deliberately NOT the published manifests' targets (which carry an
      // editor-cadence trg- range); the catalog is the trust authority for
      // offer compatibility.
      assert.deepEqual(entry.targets, { [MICROBIT_V2_LIB_COORDINATE]: { packageVersion: ">=0.2.0" } });
    }
  });

  test("Position graduates via a default-selector transport flip and is not listed as an entry", () => {
    const result = validateExtensionCatalogDocument(microbitLibraryCatalogDocument);
    assert.ok(result.ok);
    assert.equal(
      result.document.entries.find((entry) => entry.coordinate === POSITION),
      undefined,
      "Position is not a catalog entry"
    );
    const entries = result.document.moves[POSITION];
    assert.ok(entries, "the catalog declares a move for the Position coordinate");
    assert.equal(entries.length, 1);
    const [move] = entries;
    // A default-selector flip: no `from`, and the destination keeps the source coordinate.
    assert.equal(move.from, undefined);
    assert.match(move.ref, /^gh:wendoo-lang\/lib-codal-position@[0-9a-f]{40}$/);
  });

  test("each retired chassis coordinate declares a default-selector rename move onto its catalog entry's pin", () => {
    const result = validateExtensionCatalogDocument(microbitLibraryCatalogDocument);
    assert.ok(result.ok);
    const refByCoordinate = new Map(result.document.entries.map((entry) => [entry.coordinate, entry.ref]));
    for (const [retired, coordinate] of [
      [RETIRED_CUTEBOT, CUTEBOT_EXT_COORDINATE],
      [RETIRED_YAHBOOM, YAHBOOM_GAMEPAD_EXT_COORDINATE],
    ] as const) {
      const moveEntries: readonly ExtensionCatalogMoveEntry[] = result.document.moves[retired] ?? [];
      assert.equal(moveEntries.length, 1, `the catalog declares one move for ${retired}`);
      const [move] = moveEntries;
      // A rename: the default selector captures every reference of the retired
      // coordinate, and the destination is the new coordinate's entry pin.
      assert.equal(move.from, undefined);
      assert.equal(move.ref, refByCoordinate.get(coordinate));
    }
  });

  test("the startup loader throws with the stable codes when the bundled document is invalid", () => {
    assert.throws(
      () =>
        loadMicrobitLibraryCatalog({
          format: "wendoo.catalog/1",
          entries: [],
          moves: { "example-org/moved": { ref: "not-a-reference" } },
        }),
      (thrown: unknown) => thrown instanceof Error && thrown.message.includes("CATALOG_DOCUMENT_INVALID_MOVE_REF")
    );
  });
});

describe("buildMicrobitCatalogOffers -- compatibility-filtered against the micro:bit stack", () => {
  const layer: EmbeddedExtension = {
    canonicalOrigin: MICROBIT_V2_LIB_COORDINATE,
    files: [
      { path: "index.ts", content: "export {};" },
      { path: "wendoo.json", content: JSON.stringify({ name: "Micro:bit v2", version: "0.2.1" }) },
    ],
  };
  const embedRecord: readonly EmbeddedExtension[] = [layer];
  const project = { [MICROBIT_V2_LIB_COORDINATE]: MICROBIT_V2_LIB_REFERENCE };

  test("the cutebot and yahboom gh: offers are compatible with a fresh micro:bit project", () => {
    const offers = buildMicrobitCatalogOffers(project, embedRecord);
    assert.deepEqual(
      offers.map((offer) => offer.coordinate).sort(),
      [CUTEBOT_EXT_COORDINATE, YAHBOOM_GAMEPAD_EXT_COORDINATE].sort()
    );
    for (const offer of offers) {
      assert.match(offer.ref, new RegExp(`^gh:${offer.coordinate}@[0-9a-f]{40}$`));
    }
  });

  test("a project carrying an offer's coordinate drops that offer, leaving the not-installed one", () => {
    const offers = buildMicrobitCatalogOffers(
      { ...project, [CUTEBOT_EXT_COORDINATE]: `gh:${CUTEBOT_EXT_COORDINATE}@0.2.2` },
      embedRecord
    );
    assert.deepEqual(
      offers.map((offer) => offer.coordinate),
      [YAHBOOM_GAMEPAD_EXT_COORDINATE]
    );
  });

  test("the retired embedded coordinates are never offered", () => {
    const offers = buildMicrobitCatalogOffers(project, embedRecord);
    const coordinates = offers.map((offer) => offer.coordinate);
    assert.equal(coordinates.includes(RETIRED_CUTEBOT), false);
    assert.equal(coordinates.includes(RETIRED_YAHBOOM), false);
  });

  test("the shelf lists the catalog's libraries for a fresh project, none of them installed", () => {
    const shelf = buildMicrobitLibraryShelf(project, embedRecord);

    assert.deepEqual(
      shelf.map((entry) => entry.coordinate).sort(),
      [CUTEBOT_EXT_COORDINATE, YAHBOOM_GAMEPAD_EXT_COORDINATE].sort()
    );
    assert.deepEqual(
      shelf.map((entry) => entry.installed),
      [false, false]
    );
    for (const entry of shelf) {
      assert.ok(entry.description.length > 0, `${entry.coordinate} carries what it adds`);
    }
  });

  test("installing a library keeps it on the shelf and flips it to installed", () => {
    const shelf = buildMicrobitLibraryShelf(
      { ...project, [CUTEBOT_EXT_COORDINATE]: catalogEntryFor(CUTEBOT_EXT_COORDINATE).ref },
      embedRecord
    );

    assert.deepEqual(
      shelf.map((entry) => [entry.coordinate, entry.installed]).sort(),
      [
        [CUTEBOT_EXT_COORDINATE, true],
        [YAHBOOM_GAMEPAD_EXT_COORDINATE, false],
      ].sort()
    );
  });

  test("shelves the version each entry offers, never an earlier approved one", () => {
    const shelf = buildMicrobitLibraryShelf(project, embedRecord);

    for (const entry of shelf) {
      const listed = catalogEntryFor(entry.coordinate);
      assert.equal(entry.version, listed.version);
      assert.ok(listed.priors !== undefined && listed.priors.length > 0, `${entry.coordinate} has earlier versions`);
      assert.equal(
        listed.priors.some((prior) => prior.version === entry.version),
        false
      );
    }
  });
});

describe("buildMicrobitExtensionEntries -- manifest-map membership drives gh: cards", () => {
  const SHA = "0123456789abcdef0123456789abcdef01234567";
  const TOP_LIB = "example-org/top-lib";
  const TRANSITIVE_DEP = "example-org/transitive-dep";
  const topRef = `gh:${TOP_LIB}@${SHA}`;
  const transitiveRef = `gh:${TRANSITIVE_DEP}@${SHA}`;

  const microbitLayer: EmbeddedExtension = {
    canonicalOrigin: MICROBIT_V2_LIB_COORDINATE,
    files: [
      { path: "index.ts", content: "export {};" },
      { path: "wendoo.json", content: JSON.stringify({ name: "Micro:bit v2", version: "0.2.1" }) },
    ],
  };

  function manifestFiles(name: string): ReadonlyMap<string, string> {
    return new Map([["/wendoo.json", JSON.stringify({ name, version: "1.0.0" })]]);
  }

  test("lists a top-level gh: install from the map and omits a transitive gh: dep held only in the snapshot store", () => {
    const extensions = { [MICROBIT_V2_LIB_COORDINATE]: MICROBIT_V2_LIB_REFERENCE, [TOP_LIB]: topRef };
    // The transitive dep's content sits in the fetched-content snapshot store,
    // but its coordinate is NOT in the manifest extensions map.
    const installedContent: FetchedExtensionContentMap = new Map([
      [topRef, manifestFiles("Top Lib")],
      [transitiveRef, manifestFiles("Transitive Dep")],
    ]);
    const entries = buildMicrobitExtensionEntries(extensions, [microbitLayer], installedContent);
    const coordinates = entries.map((entry) => entry.coordinate);
    assert.equal(coordinates.includes(TOP_LIB), true);
    assert.equal(coordinates.includes(TRANSITIVE_DEP), false);
  });
});

describe("target/stdlib split -- browser representation of the seeded target and an installed catalog library", () => {
  /** Build an embedded extension whose bundled manifest declares the given dependency edges and compatibility targets. */
  function ext(
    coordinate: string,
    manifest: {
      version?: string;
      extensions?: Record<string, string>;
      targets?: Record<string, { packageVersion: string }>;
    }
  ): EmbeddedExtension {
    return {
      canonicalOrigin: coordinate,
      files: [
        { path: "index.ts", content: "export {};" },
        {
          path: "wendoo.json",
          content: JSON.stringify({
            name: coordinate,
            version: manifest.version ?? "0.2.1",
            ...(manifest.extensions !== undefined ? { extensions: manifest.extensions } : {}),
            ...(manifest.targets !== undefined ? { targets: manifest.targets } : {}),
          }),
        },
      ],
    };
  }

  const coreLib = ext(CORE_LIB_COORDINATE, { version: "0.2.1" });
  const codalLib = ext(CODAL_LIB_COORDINATE, {
    version: "0.2.1",
    extensions: { [CORE_LIB_COORDINATE]: `embedded:${CORE_LIB_COORDINATE}` },
  });
  const microbitV2Lib = ext(MICROBIT_V2_LIB_COORDINATE, {
    version: "0.2.1",
    extensions: { [CODAL_LIB_COORDINATE]: `embedded:${CODAL_LIB_COORDINATE}` },
  });
  // The editor/hostApp target carries no stdlib code; it declares the embedded
  // edge to the stdlib layer, so the layer resolves transitively.
  const target = ext(MICROBIT_V2_TARGET_COORDINATE, {
    version: "0.2.1",
    extensions: { [MICROBIT_V2_LIB_COORDINATE]: MICROBIT_V2_LIB_REFERENCE },
  });

  const embedRecord: readonly EmbeddedExtension[] = [target, microbitV2Lib, codalLib, coreLib];
  const YAHBOOM_SHA = "0123456789abcdef0123456789abcdef01234567";
  const yahboomRef = `gh:${YAHBOOM_GAMEPAD_EXT_COORDINATE}@${YAHBOOM_SHA}`;
  /** The installed gamepad's fetched snapshot content, as the store holds it after the install transaction. */
  const yahboomContent: FetchedExtensionContentMap = new Map([
    [yahboomRef, new Map([["/wendoo.json", JSON.stringify({ name: "Yahboom Gamepad", version: "0.2.0" })]])],
  ]);
  // A fresh project seeds only the target; the user has then installed the
  // yahboom catalog offer at its pinned gh: reference.
  const project: Readonly<Record<string, string>> = {
    [MICROBIT_V2_TARGET_COORDINATE]: MICROBIT_V2_TARGET_REFERENCE,
    [YAHBOOM_GAMEPAD_EXT_COORDINATE]: yahboomRef,
  };

  /** A persistence double recording every extensions map applied through the host. */
  function capturingPersistence(): ExtensionProjectPersistence & {
    patches: Array<Record<string, string> | undefined>;
  } {
    const patches: Array<Record<string, string> | undefined> = [];
    return {
      patches,
      updateProjectExtensions: async (extensions) => {
        patches.push(extensions);
        return {
          committed: true,
          outcome: { kind: "unchanged" as const, newProblems: [], resolvedProblems: [] },
          warnings: [],
        };
      },
    };
  }

  test("the seeded target is not an entry card, and the installed gh: library is a manageable card", () => {
    const entries = buildMicrobitExtensionEntries(project, embedRecord, yahboomContent);
    const coordinates = entries.map((entry) => entry.coordinate);
    assert.equal(coordinates.includes(MICROBIT_V2_TARGET_COORDINATE), false);
    const yahboomEntry = entries.find((entry) => entry.coordinate === YAHBOOM_GAMEPAD_EXT_COORDINATE);
    assert.ok(yahboomEntry);
    assert.equal(yahboomEntry.installed, true);
    assert.equal(yahboomEntry.updatable, true);
    assert.equal(yahboomEntry.repoUrl, `https://github.com/${YAHBOOM_GAMEPAD_EXT_COORDINATE}`);
  });

  test("the installed library is dropped from the catalog offers, while a not-installed library still offers", () => {
    const offers = buildMicrobitCatalogOffers(project, embedRecord);
    const coordinates = offers.map((offer) => offer.coordinate);
    assert.equal(coordinates.includes(YAHBOOM_GAMEPAD_EXT_COORDINATE), false);
    assert.equal(coordinates.includes(CUTEBOT_EXT_COORDINATE), true);
  });

  test("the installed gh: library uninstalls, persisting an extensions map without its coordinate", async () => {
    const persistence = capturingPersistence();
    const result = await uninstallMicrobitExtension(
      persistence,
      project,
      YAHBOOM_GAMEPAD_EXT_COORDINATE,
      embedRecord,
      yahboomContent
    );
    assert.equal(result.action.ok, true);
    assert.equal(result.action.code, ExtensionActionResultCode.UNINSTALLED);
    assert.equal(persistence.patches.length, 1);
    assert.equal(YAHBOOM_GAMEPAD_EXT_COORDINATE in (persistence.patches[0] ?? {}), false);
  });

  test("uninstalling the seeded target is refused as a locked platform coordinate", async () => {
    const persistence = capturingPersistence();
    const result = await uninstallMicrobitExtension(persistence, project, MICROBIT_V2_TARGET_COORDINATE, embedRecord);
    assert.equal(result.action.ok, false);
    assert.equal(result.action.code, ExtensionActionResultCode.LOCKED);
    assert.equal(persistence.patches.length, 0);
  });
});
