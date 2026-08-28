import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { mkdtemp, rm, rmdir, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { describe, test } from "node:test";
import { fileURLToPath, pathToFileURL } from "node:url";
import type { TargetAdapter } from "@wendoo/assistant-bridge";
import { catalogDigest } from "@wendoo/assistant-bridge";
import { environmentTiles } from "@wendoo/assistant-panel";
import { build } from "esbuild";
import { MICROBIT_V2_TARGET_COORDINATE } from "../services/microbit-extension-coordinates";

/** The app directory, from this module's own location. */
const APP_DIR = join(dirname(fileURLToPath(import.meta.url)), "..", "..");

/** The identity this target's own manifest declares. */
const targetIdentity = (
  JSON.parse(readFileSync(join(APP_DIR, "target-package", "wendoo.json"), "utf8")) as { identity: string }
).identity;

/** Device-runtime entry publishing this device's adapter factory. */
const ADAPTER_ENTRY = "@wendoo/wodal/targets/microbit-v2/rehearsal";

/** The headless adapter artifact `npm run build:headless` produces. */
const artifactPath = join(APP_DIR, "dist-headless", "rehearsal", "adapter.js");

/** The module a browser build of {@link ADAPTER_ENTRY} emits. */
async function browserBundle(): Promise<string> {
  const built = await build({
    stdin: {
      contents: `export { createTargetAdapter } from ${JSON.stringify(ADAPTER_ENTRY)};\n`,
      resolveDir: APP_DIR,
      sourcefile: "adapter-entry.js",
      loader: "js",
    },
    bundle: true,
    platform: "browser",
    format: "esm",
    write: false,
    logLevel: "silent",
  });
  return built.outputFiles.map((file) => file.text).join("\n");
}

/** Import `code` as an ES module from a directory of its own. */
async function importModule(code: string): Promise<{ createTargetAdapter: (identity: string) => TargetAdapter }> {
  const directory = await mkdtemp(join(tmpdir(), "microbit-browser-adapter-"));
  const modulePath = join(directory, "adapter.mjs");
  try {
    await writeFile(modulePath, code, "utf8");
    return (await import(pathToFileURL(modulePath).href)) as {
      createTargetAdapter: (identity: string) => TargetAdapter;
    };
  } finally {
    await rm(modulePath, { force: true });
    await rmdir(directory);
  }
}

/**
 * The digest of the tiles `adapter` installs, as a session states them to the
 * model, without the tiles any one document mints for itself.
 */
function digestOf(adapter: TargetAdapter) {
  return catalogDigest(environmentTiles(adapter));
}

describe("the tile catalog this target states to the model", () => {
  test("is stated under the identity the app mounts its adapter with", () => {
    assert.equal(MICROBIT_V2_TARGET_COORDINATE, targetIdentity);
  });

  test("digests the same through a browser build as through the headless artifact", async () => {
    const browser = (await importModule(await browserBundle())).createTargetAdapter(targetIdentity);
    const headless = (
      (await import(pathToFileURL(artifactPath).href)) as { createTargetAdapter: () => TargetAdapter }
    ).createTargetAdapter();

    assert.equal(browser.targetIdentity, targetIdentity);
    assert.equal(headless.targetIdentity, targetIdentity);

    const fromBrowser = digestOf(browser);
    const fromHeadless = digestOf(headless);

    assert.ok(fromBrowser.tileCount > 0, "the browser build states no tiles");
    assert.equal(fromBrowser.tileCount, fromHeadless.tileCount);
    assert.equal(fromBrowser.text, fromHeadless.text);
    assert.equal(fromBrowser.hash, fromHeadless.hash);
  });
});
