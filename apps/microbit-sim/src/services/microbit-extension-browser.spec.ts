import assert from "node:assert/strict";
import { describe, test } from "node:test";
import { ExtensionAddInputErrorCode, ExtensionFetchErrorCode, resolveExtensionAddInput } from "@wendoo/app-host";
import type { EmbeddedExtension, ExtensionCatalogEntry, LibraryOfferToasts } from "@wendoo/bridge-app";
import { ExtensionActionResultCode, resolveProjectExtensions } from "@wendoo/bridge-app";
import {
  addMicrobitLibrary,
  buildMicrobitExtensionEntries,
  checkMicrobitExtensionUpdates,
  type ExtensionProjectPersistence,
  type ExtensionReferenceInstallSurface,
  installMicrobitExtension,
  installMicrobitExtensionReference,
  installMicrobitReference,
  type LibraryOfferInstallHost,
  microbitLibraryCatalog,
  microbitLibraryDisplayName,
  toExtensionBrowserEntry,
  uninstallMicrobitExtension,
} from "./microbit-extension-browser";
import {
  CODAL_LIB_COORDINATE,
  CORE_LIB_COORDINATE,
  MICROBIT_V2_LIB_COORDINATE,
  MICROBIT_V2_LIB_REFERENCE,
} from "./microbit-extension-coordinates";

/** Build an embedded extension whose bundled `wendoo.json` declares the given manifest fields. */
function ext(
  coordinate: string,
  manifest: {
    name?: string;
    version?: string;
    extensions?: Record<string, string>;
    targets?: Record<string, { packageVersion: string }>;
    thumbnailUrl?: string;
  }
): EmbeddedExtension {
  return {
    canonicalOrigin: coordinate,
    files: [
      { path: "index.ts", content: "export {};" },
      {
        path: "wendoo.json",
        content: JSON.stringify({
          name: manifest.name ?? coordinate,
          version: manifest.version ?? "1.0.0",
          ...(manifest.thumbnailUrl !== undefined ? { thumbnailUrl: manifest.thumbnailUrl } : {}),
          ...(manifest.extensions !== undefined ? { extensions: manifest.extensions } : {}),
          ...(manifest.targets !== undefined ? { targets: manifest.targets } : {}),
        }),
      },
    ],
  };
}

const POSITION = "wendoo-lang/microbit-position";
const LEGACY = "wendoo-lang/legacy-widget";

const coreLib = ext(CORE_LIB_COORDINATE, { name: "Core", version: "0.2.1" });
const wodalLib = ext(CODAL_LIB_COORDINATE, {
  name: "Wodal",
  version: "0.2.1",
  extensions: { [CORE_LIB_COORDINATE]: `embedded:${CORE_LIB_COORDINATE}` },
});
const microbitLib = ext(MICROBIT_V2_LIB_COORDINATE, {
  name: "Micro:bit v2",
  version: "0.2.1",
  extensions: { [CODAL_LIB_COORDINATE]: `embedded:${CODAL_LIB_COORDINATE}` },
});
/** A micro:bit-compatible add-on carrying a thumbnail. */
const positionAddon = ext(POSITION, {
  name: "Position",
  version: "1.3.0",
  thumbnailUrl: "data:,pos",
  targets: { [MICROBIT_V2_LIB_COORDINATE]: { packageVersion: "^0.2.0" } },
});
/** An add-on whose micro:bit target is at a version the stack excludes. */
const legacyAddon = ext(LEGACY, {
  name: "Legacy Widget",
  version: "1.0.0",
  targets: { [MICROBIT_V2_LIB_COORDINATE]: { packageVersion: "^0.1.0" } },
});

const embedRecord: readonly EmbeddedExtension[] = [microbitLib, wodalLib, coreLib, positionAddon, legacyAddon];
const project = { [MICROBIT_V2_LIB_COORDINATE]: MICROBIT_V2_LIB_REFERENCE };

/** A persistence double capturing every extensions map applied through the host. */
function capturingPersistence(): ExtensionProjectPersistence & { patches: Array<Record<string, string> | undefined> } {
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

describe("buildMicrobitExtensionEntries -- direct dependencies adapted to browser entries", () => {
  test("lists nothing for a fresh project: the platform layer is not an entry card", () => {
    const entries = buildMicrobitExtensionEntries(project, embedRecord);
    assert.deepEqual(
      entries.map((e) => e.coordinate),
      []
    );
  });

  test("lists a directly-installed embedded add-on as an installed entry with no repository URL", () => {
    const withPosition = { ...project, [POSITION]: `embedded:${POSITION}` };
    const entries = buildMicrobitExtensionEntries(withPosition, embedRecord);
    assert.deepEqual(
      entries.map((e) => e.coordinate),
      [POSITION]
    );

    const position = entries.find((e) => e.coordinate === POSITION);
    assert.ok(position);
    assert.equal(position.installed, true);
    assert.equal(position.name, "Position");
    assert.equal(position.thumbnailUrl, "data:,pos");
    // An embedded add-on's coordinate is not a GitHub repository, so it carries no repoUrl.
    assert.equal("repoUrl" in position, false);
  });

  test("excludes the platform layer, transitive layer libs, and every non-referenced bundled add-on", () => {
    const entries = buildMicrobitExtensionEntries(project, embedRecord);
    const coordinates = entries.map((e) => e.coordinate);
    assert.equal(coordinates.includes(MICROBIT_V2_LIB_COORDINATE), false);
    assert.equal(coordinates.includes(CORE_LIB_COORDINATE), false);
    assert.equal(coordinates.includes(CODAL_LIB_COORDINATE), false);
    assert.equal(coordinates.includes(POSITION), false);
    assert.equal(coordinates.includes(LEGACY), false);
  });
});

describe("buildMicrobitExtensionEntries -- a platform reached through target edges stays non-manageable", () => {
  // The layers below the referenced platform chain through `targets`, as the
  // migrated platform manifests do: micro:bit v2 --targets--> wodal --targets--> core.
  const microbitLibViaTargets = ext(MICROBIT_V2_LIB_COORDINATE, {
    name: "Micro:bit v2",
    version: "0.2.1",
    targets: { [CODAL_LIB_COORDINATE]: { packageVersion: "^0.2.0" } },
  });
  const wodalLibViaTargets = ext(CODAL_LIB_COORDINATE, {
    name: "Wodal",
    version: "0.2.1",
    targets: { [CORE_LIB_COORDINATE]: { packageVersion: "^0.2.0" } },
  });
  const targetsEmbedRecord: readonly EmbeddedExtension[] = [microbitLibViaTargets, wodalLibViaTargets, coreLib];

  test("the layers materialized through target edges are not entry cards", () => {
    const entries = buildMicrobitExtensionEntries(project, targetsEmbedRecord);
    const coordinates = entries.map((e) => e.coordinate);
    assert.equal(coordinates.includes(MICROBIT_V2_LIB_COORDINATE), false);
    assert.equal(coordinates.includes(CODAL_LIB_COORDINATE), false);
    assert.equal(coordinates.includes(CORE_LIB_COORDINATE), false);
  });

  test("a layer reached through a target edge cannot be uninstalled", async () => {
    const persistence = capturingPersistence();
    const result = await uninstallMicrobitExtension(persistence, project, CODAL_LIB_COORDINATE, targetsEmbedRecord);
    assert.equal(result.action.ok, false);
    assert.equal(result.action.code, ExtensionActionResultCode.LOCKED);
    assert.equal(persistence.patches.length, 0);
  });

  test("a platform layer listed in the resolved dependencies is still not an entry card", () => {
    // The target-recursed layers join the project's importable dependencies,
    // yet the entry cards derive from the extensions map and the platform set.
    const resolved = resolveProjectExtensions(project, { embedded: targetsEmbedRecord });
    const dependencyCoordinates = resolved.dependencies.map((dependency) => dependency.coordinate);
    assert.ok(dependencyCoordinates.includes(CODAL_LIB_COORDINATE));
    assert.ok(dependencyCoordinates.includes(CORE_LIB_COORDINATE));

    const coordinates = buildMicrobitExtensionEntries(project, targetsEmbedRecord).map((entry) => entry.coordinate);
    assert.equal(coordinates.includes(MICROBIT_V2_LIB_COORDINATE), false);
    assert.equal(coordinates.includes(CODAL_LIB_COORDINATE), false);
    assert.equal(coordinates.includes(CORE_LIB_COORDINATE), false);
  });
});

describe("toExtensionBrowserEntry", () => {
  test("carries a repository URL and thumbnail through when the catalog entry declares them", () => {
    const catalogEntry: ExtensionCatalogEntry = {
      coordinate: POSITION,
      name: "Position",
      version: "1.3.0",
      thumbnailUrl: "data:,pos",
      installed: false,
      repoUrl: `https://github.com/${POSITION}`,
    };
    assert.deepEqual(toExtensionBrowserEntry(catalogEntry), {
      coordinate: POSITION,
      name: "Position",
      version: "1.3.0",
      thumbnailUrl: "data:,pos",
      installed: false,
      repoUrl: `https://github.com/${POSITION}`,
    });
  });

  test("omits the repository URL when the catalog entry declares none", () => {
    const catalogEntry: ExtensionCatalogEntry = {
      coordinate: POSITION,
      name: "Position",
      version: "1.3.0",
      installed: false,
    };
    assert.equal("repoUrl" in toExtensionBrowserEntry(catalogEntry), false);
  });

  test("omits the thumbnail when the catalog entry declares none", () => {
    const catalogEntry: ExtensionCatalogEntry = {
      coordinate: MICROBIT_V2_LIB_COORDINATE,
      name: "Micro:bit v2",
      version: "0.2.1",
      installed: true,
    };
    assert.equal("thumbnailUrl" in toExtensionBrowserEntry(catalogEntry), false);
  });
});

describe("installMicrobitExtension -- round-trips through the host", () => {
  test("installing an add-on persists an extensions map that gains the coordinate", async () => {
    const persistence = capturingPersistence();
    const result = await installMicrobitExtension(persistence, project, POSITION, embedRecord);
    assert.equal(result.action.ok, true);
    assert.equal(result.action.code, ExtensionActionResultCode.INSTALLED);
    assert.equal(persistence.patches.length, 1);
    assert.equal(persistence.patches[0]?.[POSITION], `embedded:${POSITION}`);
    assert.equal(persistence.patches[0]?.[MICROBIT_V2_LIB_COORDINATE], MICROBIT_V2_LIB_REFERENCE);
  });

  test("installing an already-present coordinate does not persist", async () => {
    const persistence = capturingPersistence();
    const result = await installMicrobitExtension(persistence, project, MICROBIT_V2_LIB_COORDINATE, embedRecord);
    assert.equal(result.action.ok, false);
    assert.equal(result.action.code, ExtensionActionResultCode.ALREADY_INSTALLED);
    assert.equal(persistence.patches.length, 0);
  });
});

describe("uninstallMicrobitExtension -- round-trips through the host", () => {
  const withPosition = { ...project, [POSITION]: `embedded:${POSITION}` };

  test("uninstalling an add-on persists an extensions map that loses the coordinate", async () => {
    const persistence = capturingPersistence();
    const result = await uninstallMicrobitExtension(persistence, withPosition, POSITION, embedRecord);
    assert.equal(result.action.ok, true);
    assert.equal(result.action.code, ExtensionActionResultCode.UNINSTALLED);
    assert.equal(persistence.patches.length, 1);
    assert.equal(POSITION in (persistence.patches[0] ?? {}), false);
    assert.equal(persistence.patches[0]?.[MICROBIT_V2_LIB_COORDINATE], MICROBIT_V2_LIB_REFERENCE);
  });

  test("uninstalling a locked layer library is rejected and does not persist", async () => {
    const persistence = capturingPersistence();
    const result = await uninstallMicrobitExtension(persistence, project, MICROBIT_V2_LIB_COORDINATE, embedRecord);
    assert.equal(result.action.ok, false);
    assert.equal(result.action.code, ExtensionActionResultCode.LOCKED);
    assert.equal(persistence.patches.length, 0);
  });

  test("uninstalling a coordinate a still-installed add-on depends on is rejected and does not persist", async () => {
    const persistence = capturingPersistence();
    // A gamepad add-on that depends on the Position add-on, both installed.
    const GAMEPAD = "wendoo-lang/microbit-gamepad";
    const gamepadAddon = ext(GAMEPAD, {
      name: "Gamepad",
      version: "1.0.0",
      targets: { [MICROBIT_V2_LIB_COORDINATE]: { packageVersion: "^0.2.0" } },
      extensions: { [POSITION]: `embedded:${POSITION}` },
    });
    const withDependent = { ...withPosition, [GAMEPAD]: `embedded:${GAMEPAD}` };
    const result = await uninstallMicrobitExtension(persistence, withDependent, POSITION, [
      ...embedRecord,
      gamepadAddon,
    ]);
    assert.equal(result.action.ok, false);
    assert.equal(result.action.code, ExtensionActionResultCode.REQUIRED_BY_DEPENDENT);
    assert.equal(persistence.patches.length, 0);
  });
});

/**
 * An install surface running real input normalization over a stub version
 * listing, capturing every extensions map applied through the host.
 */
function referenceInstallSurface(
  versions: Record<string, readonly string[]> = {}
): ExtensionReferenceInstallSurface & { patches: Array<Record<string, string> | undefined> } {
  return {
    ...capturingPersistence(),
    resolveExtensionInstallInput: (input: string) =>
      resolveExtensionAddInput(input, {
        async fetchFile() {
          return { ok: false, kind: "not-found" };
        },
        async resolveBranch() {
          return { ok: false, kind: "not-found" };
        },
        async listVersionTags(owner: string, repo: string) {
          const listed = versions[`${owner}/${repo}`];
          return listed !== undefined ? { ok: true, versions: listed } : { ok: false, kind: "not-found" };
        },
      }),
  };
}

describe("installMicrobitExtensionReference -- generous input through the host", () => {
  test("adding a complete gh reference persists it unchanged, keyed by its coordinate", async () => {
    const surface = referenceInstallSurface();
    const result = await installMicrobitExtensionReference(surface, project, "gh:example-org/position-ext@v0.1.0");
    assert.ok(result.ok);
    assert.equal(result.reference, "gh:example-org/position-ext@v0.1.0");
    assert.equal(result.action.ok, true);
    assert.equal(result.action.code, ExtensionActionResultCode.INSTALLED);
    assert.ok(result.report);
    assert.equal(surface.patches.length, 1);
    assert.equal(surface.patches[0]?.["example-org/position-ext"], "gh:example-org/position-ext@v0.1.0");
  });

  test("pasting a GitHub repository URL resolves the latest published version and persists the resolved reference", async () => {
    const surface = referenceInstallSurface({ "example-org/position-ext": ["0.1.0", "0.2.0"] });
    const result = await installMicrobitExtensionReference(
      surface,
      project,
      "https://github.com/example-org/position-ext"
    );
    assert.ok(result.ok);
    assert.equal(result.reference, "gh:example-org/position-ext@0.2.0");
    assert.equal(result.action.ok, true);
    assert.equal(surface.patches.length, 1);
    assert.equal(surface.patches[0]?.["example-org/position-ext"], "gh:example-org/position-ext@0.2.0");
  });

  test("a repository with no published versions is rejected with its code and does not persist", async () => {
    const surface = referenceInstallSurface();
    const result = await installMicrobitExtensionReference(surface, project, "example-org/position-ext");
    assert.ok(!result.ok);
    assert.equal(result.code, ExtensionFetchErrorCode.VERSIONS_NOT_FOUND);
    assert.equal(surface.patches.length, 0);
  });

  test("unrecognized input is rejected with its code and does not persist", async () => {
    const surface = referenceInstallSurface();
    const result = await installMicrobitExtensionReference(surface, project, "ffff:x");
    assert.ok(!result.ok);
    assert.equal(result.code, ExtensionAddInputErrorCode.UNRECOGNIZED);
    assert.equal(surface.patches.length, 0);
  });
});

describe("toExtensionBrowserEntry -- fetched-dependency annotations", () => {
  test("passes updatable, broken, and identityMismatch through to the view model", () => {
    const catalogEntry: ExtensionCatalogEntry = {
      coordinate: "example-org/position-ext",
      name: "Position",
      version: "0.1.0",
      installed: true,
      updatable: true,
      broken: { code: "EXTENSION_FETCH_UNREACHABLE", message: "The source is unreachable: refused" },
      identityMismatch: { declaredIdentity: "upstream-org/position-ext" },
    };
    const entry = toExtensionBrowserEntry(catalogEntry);
    assert.equal(entry.updatable, true);
    assert.deepEqual(entry.broken, {
      code: "EXTENSION_FETCH_UNREACHABLE",
      message: "The source is unreachable: refused",
    });
    assert.deepEqual(entry.identityMismatch, { declaredIdentity: "upstream-org/position-ext" });
  });
});

describe("checkMicrobitExtensionUpdates", () => {
  test("buckets available updates, current dependencies, and failed checks", async () => {
    const surface = {
      checkExtensionUpdate: async (coordinate: string) => {
        if (coordinate === "example-org/current-ext") {
          return { ok: true as const, updateAvailable: false as const };
        }
        if (coordinate === "example-org/stale-ext") {
          return {
            ok: true as const,
            updateAvailable: true as const,
            update: {
              coordinate,
              reference: "gh:example-org/stale-ext@0.2.0",
              latestVersion: "0.2.0",
            },
          };
        }
        return {
          ok: false as const,
          error: {
            code: "EXTENSION_FETCH_UNREACHABLE" as const,
            reference: coordinate,
            message: "The source is unreachable: refused",
          },
        };
      },
    };

    const summary = await checkMicrobitExtensionUpdates(surface, [
      "example-org/current-ext",
      "example-org/stale-ext",
      "example-org/offline-ext",
    ]);

    assert.deepEqual(summary.current, ["example-org/current-ext"]);
    assert.deepEqual(
      summary.updates.map((update) => update.reference),
      ["gh:example-org/stale-ext@0.2.0"]
    );
    assert.equal(summary.failures.length, 1);
    assert.equal(summary.failures[0].coordinate, "example-org/offline-ext");
    assert.equal(summary.failures[0].error.code, "EXTENSION_FETCH_UNREACHABLE");
  });
});

describe("installMicrobitReference -- routes by transport", () => {
  test("an embedded offer ref installs by writing embedded:<coord> to the map", async () => {
    const surface = referenceInstallSurface();
    const result = await installMicrobitReference(surface, project, embedRecord, `embedded:${POSITION}`);
    assert.ok(result.ok);
    assert.equal(result.action.ok, true);
    assert.equal(result.action.code, ExtensionActionResultCode.INSTALLED);
    assert.equal(result.action.extensions[POSITION], `embedded:${POSITION}`);
    assert.equal(surface.patches[0]?.[POSITION], `embedded:${POSITION}`);
  });

  test("a gh reference routes through the remote installer and writes gh:", async () => {
    const surface = referenceInstallSurface();
    const result = await installMicrobitReference(surface, project, embedRecord, "gh:example-org/position-ext@v0.1.0");
    assert.ok(result.ok);
    assert.equal(result.action.ok, true);
    assert.equal(surface.patches[0]?.["example-org/position-ext"], "gh:example-org/position-ext@v0.1.0");
  });
});

describe("microbitLibraryDisplayName", () => {
  test("prefers the installed library's manifest name", () => {
    const name = microbitLibraryDisplayName([{ coordinate: POSITION, name: "Position" }], POSITION);
    assert.equal(name, "Position");
  });

  test("falls back to the bundled catalog entry's name when not installed", () => {
    const entry = microbitLibraryCatalog.entries[0];
    assert.ok(entry, "the bundled catalog carries at least one entry");
    const name = microbitLibraryDisplayName([], entry.coordinate);
    assert.equal(name, entry.name);
  });

  test("falls back to the coordinate when nothing names the library", () => {
    const name = microbitLibraryDisplayName([], "example-org/unknown-lib");
    assert.equal(name, "example-org/unknown-lib");
  });
});

describe("addMicrobitLibrary -- an offer the assistant made, added through the app's own install", () => {
  /** The first library the bundled catalog shelves, which every offer here names. */
  const shelved = microbitLibraryCatalog.entries[0];

  /** An install host over the real normalization surface, naming nothing installed yet. */
  function offerHost(): LibraryOfferInstallHost & { patches: Array<Record<string, string> | undefined> } {
    return { ...referenceInstallSurface(), installedLibraries: [] };
  }

  /** A toast surface recording the kind of every outcome presented through it. */
  function recordingToasts(): { toasts: LibraryOfferToasts; kinds: string[] } {
    const kinds: string[] = [];
    return {
      kinds,
      toasts: {
        failed: () => kinds.push("failed"),
        confirmed: () => kinds.push("confirmed"),
        worsened: () => kinds.push("worsened"),
      },
    };
  }

  test("installs the reference the bundled catalog approves, through the transaction the browser uses", async () => {
    assert.ok(shelved, "the bundled catalog shelves at least one library");
    const host = offerHost();
    const { toasts, kinds } = recordingToasts();

    const held = await addMicrobitLibrary(host, project, embedRecord, shelved.coordinate, toasts);

    assert.equal(held, true);
    assert.equal(host.patches.length, 1);
    assert.equal(host.patches[0]?.[shelved.coordinate], shelved.ref);
    assert.deepEqual(kinds, ["confirmed"]);
  });

  test("refuses a coordinate the bundled catalog shelves nothing for, persisting nothing", async () => {
    const host = offerHost();
    const { toasts, kinds } = recordingToasts();

    const held = await addMicrobitLibrary(host, project, embedRecord, "example-org/unshelved-lib", toasts);

    assert.equal(held, false);
    assert.deepEqual(host.patches, []);
    assert.deepEqual(kinds, ["failed"]);
  });
});
