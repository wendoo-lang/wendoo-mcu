#!/usr/bin/env node
/**
 * Builds this target's headless adapter artifact: bundles the rehearsal adapter
 * its device-runtime dependency publishes into one self-contained,
 * plain-Node-importable ES module, published under the target identity this
 * target's own wendoo.json declares. Exits nonzero when a package the bundle
 * would carry was built before its own sources were last edited, when the
 * manifest declares no identity, or when the bundle fails.
 * Run through `npm run build:headless`.
 */
import { mkdirSync, rmSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";
import {
  assertDependencyDistsFresh,
  createTargetBuildStamp,
  readTargetIdentity,
  StaleDependencyError,
} from "@wendoo/assistant-bridge/kit/node";
import { build } from "esbuild";

/** The device this app distributes, named as its device-runtime package subtree carries it. */
const DEVICE = "microbit-v2";

/** Device-runtime entry publishing this device's rehearsal adapter factory. */
const ADAPTER_ENTRY = `@wendoo/wodal/targets/${DEVICE}/rehearsal`;

const appDir = join(dirname(fileURLToPath(import.meta.url)), "..");

try {
  assertDependencyDistsFresh(appDir);
} catch (cause) {
  if (!(cause instanceof StaleDependencyError)) throw cause;
  console.error(`build-headless-adapter: ${cause.message}`);
  process.exit(1);
}

const targetIdentity = readTargetIdentity(appDir);

/** The module the artifact publishes: the device's adapter under this target's identity, stamped with the language build it bundles. */
const artifactEntry = [
  `import { createTargetAdapter as createDeviceAdapter } from ${JSON.stringify(ADAPTER_ENTRY)};`,
  `export const createTargetAdapter = () => createDeviceAdapter(${JSON.stringify(targetIdentity)});`,
  `export const buildStamp = ${JSON.stringify(createTargetBuildStamp(appDir))};`,
  "",
].join("\n");

const artifactDir = join(appDir, "dist-headless");
const artifactPath = join(artifactDir, "rehearsal", "adapter.js");

rmSync(artifactDir, { recursive: true, force: true });
mkdirSync(dirname(artifactPath), { recursive: true });

await build({
  stdin: { contents: artifactEntry, resolveDir: appDir, sourcefile: "adapter-entry.js", loader: "js" },
  outfile: artifactPath,
  bundle: true,
  platform: "node",
  format: "esm",
  logLevel: "warning",
});

console.log(`built headless adapter: ${targetIdentity} at ${relative(appDir, artifactPath)}`);
