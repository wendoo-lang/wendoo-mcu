import assert from "node:assert/strict";
import { describe, test } from "node:test";
import { fileURLToPath } from "node:url";
import type { EmbeddedExtension } from "@wendoo/bridge-app";
import { deriveProjectPlatformStack, resolveProjectExtensions } from "@wendoo/bridge-app";
import { buildEmbeddedExtensionFromDir } from "@wendoo/bridge-app/node";
import { buildMicrobitCatalogOffers, MICROBIT_LAYER_COORDINATES } from "./microbit-extension-browser";
import {
  CODAL_LIB_COORDINATE,
  CORE_LIB_COORDINATE,
  CUTEBOT_EXT_COORDINATE,
  MICROBIT_V2_LIB_COORDINATE,
  MICROBIT_V2_TARGET_COORDINATE,
  MICROBIT_V2_TARGET_REFERENCE,
  YAHBOOM_GAMEPAD_EXT_COORDINATE,
} from "./microbit-extension-coordinates";

function extensionDir(relativePath: string): string {
  return fileURLToPath(new URL(relativePath, import.meta.url));
}

/**
 * The microbit-sim embed record exactly as the Vite provider assembles it: the
 * thin editor/hostApp target and the three platform layers below it. The target
 * manifest carries no standard-library content; it declares an embedded
 * dependency on the micro:bit v2 standard-library layer, so the standard
 * library resolves transitively through the target's closure.
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

/** A fresh microbit-sim project references the target alone, exactly as seeded. */
const seededProject = { [MICROBIT_V2_TARGET_COORDINATE]: MICROBIT_V2_TARGET_REFERENCE };

describe("microbit target/stdlib split -- the target pulls the standard library transitively", () => {
  test("seeding the target resolves both the target and the standard-library layer into the closure", () => {
    const resolved = resolveProjectExtensions(seededProject, { embedded: microbitEmbedRecord() });
    const origins = resolved.dependencyMounts.map((m) => m.namespace);
    assert.ok(origins.includes(MICROBIT_V2_TARGET_COORDINATE), "the target is in the closure");
    assert.ok(origins.includes(MICROBIT_V2_LIB_COORDINATE), "the standard-library layer is pulled in transitively");
    assert.ok(origins.includes(CODAL_LIB_COORDINATE), "the wodal layer is pulled in transitively");
    assert.ok(origins.includes(CORE_LIB_COORDINATE), "the core layer is pulled in transitively");
  });

  test("the platform stack reports the standard-library layer at its own version, not the target", () => {
    const stack = deriveProjectPlatformStack(seededProject, microbitEmbedRecord(), MICROBIT_LAYER_COORDINATES);

    const stdlib = stack.find((layer) => layer.coordinate === MICROBIT_V2_LIB_COORDINATE);
    assert.ok(stdlib, "the standard-library layer is in the stack");
    assert.equal(stdlib.version, "0.2.1");

    // The editor/hostApp target is not a compatibility layer, so it never enters the stack.
    assert.equal(
      stack.some((layer) => layer.coordinate === MICROBIT_V2_TARGET_COORDINATE),
      false
    );
  });

  test("the compatible catalog offers include the published feature libs whose curated targets name the stdlib layer", () => {
    const offers = buildMicrobitCatalogOffers(seededProject, microbitEmbedRecord());
    const coordinates = offers.map((offer) => offer.coordinate);
    assert.ok(coordinates.includes(CUTEBOT_EXT_COORDINATE), "the Cutebot offer is compatible and listed");
    assert.ok(
      coordinates.includes(YAHBOOM_GAMEPAD_EXT_COORDINATE),
      "the Yahboom gamepad offer is compatible and listed"
    );
    for (const offer of offers) {
      assert.match(offer.ref, new RegExp(`^gh:${offer.coordinate}@[0-9a-f]{40}$`), "offers install pinned gh: refs");
    }
  });
});
