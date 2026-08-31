/**
 * Golden for the TS user-code draw API (`ctx.microbit.display.drawImage`): a
 * user-tile brain whose async actuator builds an `Image` inline (a `Buffer.from`
 * pixel buffer in a struct literal) and awaits `drawImage`, the first asynchronous
 * `ctx.microbit.*` host function (op 41 `HOST_CALL_ASYNC`). Two fixtures pin the
 * display-lease behavior reached through the host-function path:
 *
 * - timed: a positive-duration draw holds the display lease for its duration; the
 *   actuator parks on the awaited handle and resumes on the first think past the
 *   hold, then lights a marker pixel through `setPixelValue`.
 * - fire-and-forget: an explicit zero-duration draw paints, takes no lease, and
 *   resolves at dispatch; the actuator continues in the same think and lights its
 *   marker pixel without ever parking.
 *
 * The rule fires once on page entry (the core `on page entered` host sensor); its
 * `do` is the compiled async actuator, whose `drawImage` / `setPixelValue` cross
 * the display port as host functions, which carry no host-action dispatch line.
 * The serialized binary and the rendered trace are pinned beside this spec; the
 * C++ VM parity test (cpp/test/trace-parity.test.cpp) loads the same binary,
 * replays the schedule, and byte-compares.
 */

import assert from "node:assert/strict";
import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { test } from "node:test";
import { fileURLToPath } from "node:url";
import { CoreHostActions, mkSensorTileId, type WendooEnvironment } from "@wendoo/core/app";
import type { IBrainTileDef } from "@wendoo/core/brain";
import { BrainDef } from "@wendoo/core/brain/model";
import {
  BrainRuntime,
  type LinkedBrainProgram,
  linkedBrainProgramToJson,
  type PlatformServices,
} from "@wendoo/core/runtime";
import {
  type AmbientFile,
  buildCompiledActionBundle,
  type DependencyMount,
  type ProjectDependency,
  UserTileProject,
} from "@wendoo/ts-compiler";
import { TEST_PROJECT_NAMESPACE } from "@wendoo/ts-compiler/testing";
import { buildWodalProgramImage } from "../../../wendoo/build-kernel";
import { getWodalDeviceProfile, WodalDeviceProfileId } from "../../../wendoo/device-profile";
import { shouldWriteGolden } from "../../../wendoo/golden-regeneration";
import { serializeWodalProgramImageJson, type WodalProgramImage } from "../../../wendoo/program-image";
import { parseWodalProgramImageBytes, wodalProgramBytes } from "../../../wendoo/program-image-binary";
import { MicroBit } from "../microbit";
import { createMicroBitV2Environment } from "./environment";
import { ObservableTraceWriter, observableTraceVmEvents } from "./observable-trace";

/** Milliseconds advanced per scheduled think. */
const TICK_ADVANCE_MS = 100;

/** Hold of the timed fixture, in seconds, passed as the `drawImage` `duration` option. */
const HOLD_SECONDS = 0.25;

/** Trace hex of the drawn 5x5 image (top row lit, the rest dark), row-major brightness bytes. */
const TOP_ROW_HEX = `ffffffffff${"00".repeat(20)}`;

/** Trace hex of the stdlib `heart` icon (5x5, row-major brightness bytes). */
const HEART_HEX = "00ff00ff00ffffffffffffffffffff00ffffff000000ff0000";

/**
 * Source of an async actuator that builds a 5x5 `Image` whose top row is lit,
 * draws it for `durationLiteral` seconds, then lights pixel (4,4) once the draw
 * resolves. With a positive duration the actuator parks until the hold elapses;
 * with an explicit `0` it continues in the same think.
 */
function actuatorSource(name: string, durationLiteral: string): string {
  return `import { Actuator, type Context, type Image } from "wendoo";

export default Actuator({
  name: "${name}",
  async onExecute(ctx: Context): Promise<void> {
    const image: Image = {
      width: 5,
      height: 5,
      pixels: Buffer.from([
        255, 255, 255, 255, 255,
        0, 0, 0, 0, 0,
        0, 0, 0, 0, 0,
        0, 0, 0, 0, 0,
        0, 0, 0, 0, 0,
      ]),
    };
    await ctx.microbit.display.drawImage(image, { duration: ${durationLiteral} });
    ctx.microbit.display.setPixelValue(4, 4, 255);
  },
});
`;
}

/**
 * Source of an async actuator that imports the lazy `heart` named-icon builder
 * from the target stdlib, constructs the icon `Image` once at module-init via
 * the stdlib's `image()` parser (string split / char access / `Buffer.from`),
 * and fire-and-forget-draws it.
 */
function iconActuatorSource(name: string): string {
  return `import { Actuator, type Context } from "wendoo";
import { heart } from "@lib/wendoo-lang/lib-microbit-v2";

const heartIcon = heart();

export default Actuator({
  name: "${name}",
  async onExecute(ctx: Context): Promise<void> {
    await ctx.microbit.display.drawImage(heartIcon, { duration: 0 });
  },
});
`;
}

function readText(relativePath: string): string {
  return readFileSync(fileURLToPath(new URL(relativePath, import.meta.url)), "utf8");
}

function wodalAmbientFiles(): readonly AmbientFile[] {
  return [
    {
      path: "wendoo.core.d.ts",
      content: readText("../../../../../../external/wendoo-lang/packages/core/lib/wendoo.core.d.ts"),
    },
    { path: "wendoo.codal.d.ts", content: readText("../../../../lib/wendoo.codal.d.ts") },
    {
      path: "wendoo.microbit-v2.d.ts",
      content: readText("../../../../targets/microbit-v2/lib/wendoo.microbit-v2.d.ts"),
    },
  ];
}

/** Canonical `<owner>/<repo>` coordinate the micro:bit v2 image library is mounted under. */
const MICROBIT_V2_LIB_COORDINATE = "wendoo-lang/lib-microbit-v2";

/** The micro:bit v2 image library dependency: `@lib/wendoo-lang/lib-microbit-v2` resolves to its entry module. */
function wodalStdlibDependencies(): readonly ProjectDependency[] {
  return [{ coordinate: MICROBIT_V2_LIB_COORDINATE }];
}

/** The micro:bit v2 image library content, mounted read-only for `@lib/wendoo-lang/lib-microbit-v2` resolution. */
function wodalStdlibDependencyMounts(): readonly DependencyMount[] {
  return [
    {
      namespace: MICROBIT_V2_LIB_COORDINATE,
      files: new Map([
        ["/index.ts", readText("../../../../targets/microbit-v2/lib/index.ts")],
        ["/image.ts", readText("../../../../targets/microbit-v2/lib/image.ts")],
        ["/sounds.ts", readText("../../../../targets/microbit-v2/lib/sounds.ts")],
        ["/waveforms.ts", readText("../../../../targets/microbit-v2/lib/waveforms.ts")],
      ]),
    },
  ];
}

function findActuatorTile(tiles: readonly IBrainTileDef[]): IBrainTileDef {
  const tile = tiles.find((candidate) => candidate.kind === "actuator");
  assert.ok(tile);
  return tile;
}

function hostServicesOf(environment: WendooEnvironment): Omit<PlatformServices, "brain"> {
  const { runtime, shared, app } = environment.brainServices;
  return { runtime, shared, app };
}

/**
 * Compiles the async draw actuator, installs it, and builds a single-page brain
 * whose rule fires on page entry (the core `on page entered` sensor) and runs the
 * actuator.
 */
function buildImage(
  environment: WendooEnvironment,
  actuatorName: string,
  source: string
): WodalProgramImage<LinkedBrainProgram> {
  const project = new UserTileProject({
    projectNamespace: TEST_PROJECT_NAMESPACE,
    ambientFiles: wodalAmbientFiles(),
    dependencies: wodalStdlibDependencies(),
    dependencyMounts: wodalStdlibDependencyMounts(),
    services: environment.brainServices,
  });
  project.setFiles(new Map([[`${actuatorName}.ts`, source]]));
  const compileResult = project.compileAll();
  assert.equal(
    compileResult.tsErrors.size,
    0,
    `Unexpected TypeScript diagnostics: ${JSON.stringify([...compileResult.tsErrors])}`
  );
  const bundle = buildCompiledActionBundle(compileResult, { services: environment.brainServices });
  assert.ok(bundle);
  environment.replaceActionBundle(bundle);

  const onPageEntered = environment.brainServices.edit.tiles.get(mkSensorTileId(CoreHostActions.OnPageEntered.key));
  assert.ok(onPageEntered, "on page entered sensor tile should be registered");

  const brainDef = BrainDef.emptyBrainDef(environment.brainServices, "user-tile draw brain");
  const rule = brainDef.pages().get(0)!.children().get(0)!;
  rule.when().appendTile(onPageEntered);
  rule.do().appendTile(findActuatorTile(bundle.tiles));

  const built = buildWodalProgramImage({
    brainDef,
    environment,
    deviceProfile: getWodalDeviceProfile(WodalDeviceProfileId.MICROBIT_V2),
  });
  if (!built.ok) {
    assert.fail(`expected a successful build: ${JSON.stringify(built.errors)}`);
  }
  return built.image;
}

/** Writes the JSON `.mcprogram` golden if missing, freezing the brain's generated id. */
function ensureJsonGolden(jsonPath: string, actuatorName: string, source: string): void {
  if (existsSync(jsonPath)) {
    return;
  }
  const environment = createMicroBitV2Environment();
  const image = buildImage(environment, actuatorName, source);
  writeFileSync(
    jsonPath,
    serializeWodalProgramImageJson({ ...image, program: linkedBrainProgramToJson(image.program) })
  );
}

/**
 * Runs the committed binary over `tickCount` thinks at {@link TICK_ADVANCE_MS}
 * each with the trace observers installed: the on-page-entered host sensor (its async
 * actuator draws and writes through host functions, which carry no host-action
 * dispatch line) plus the display draw / set-pixel ports. The draw port line is
 * emitted only when the display is free, and the display poll runs after each
 * think so a timed draw's hold settles and the awaiting fiber resumes.
 */
function runTrace(bin: Uint8Array, tickCount: number): { trace: string; microbit: MicroBit } {
  const environment = createMicroBitV2Environment();
  const profile = getWodalDeviceProfile(WodalDeviceProfileId.MICROBIT_V2);
  const decoded = parseWodalProgramImageBytes(
    bin,
    WodalDeviceProfileId.MICROBIT_V2,
    environment.brainServices.runtime.types
  );
  const writer = new ObservableTraceWriter({ profileId: profile.numericProfileId, precision: profile.numberPrecision });

  const microbit = new MicroBit();
  const devicePaintFrame = microbit.display.paintFrame.bind(microbit.display);
  microbit.display.paintFrame = (image) => {
    writer.displayDraw(image.width, image.height, image.frame);
    devicePaintFrame(image);
  };
  const deviceSetPixelValue = microbit.display.setPixelValue.bind(microbit.display);
  microbit.display.setPixelValue = (x, y, brightness) => {
    writer.displaySetPixel(x, y, brightness);
    deviceSetPixelValue(x, y, brightness);
  };

  const vmEvents = observableTraceVmEvents(writer);

  const linked = decoded.program;
  const brain = new BrainRuntime(
    linked.program,
    linked.pages,
    hostServicesOf(environment),
    { microbit },
    undefined,
    vmEvents,
    {
      defaultBudget: profile.defaultBudget,
      hookBudget: profile.hookBudget,
      maxFibers: profile.maxFibers,
      maxStackSize: profile.maxStackSize,
      maxLocalsSize: profile.maxLocalsSize,
      maxFrameDepth: profile.maxFrameDepth,
      maxHandlers: profile.maxHandlers,
    }
  );
  brain.startup();

  let lastThinkTimeMs = 0;
  for (let i = 0; i < tickCount; i++) {
    const timeMs = lastThinkTimeMs + TICK_ADVANCE_MS;
    writer.tick(i + 1, timeMs, lastThinkTimeMs === 0 ? 0 : timeMs - lastThinkTimeMs);
    brain.think(timeMs);
    microbit.display.advanceScroll(timeMs);
    lastThinkTimeMs = timeMs;
  }
  return { trace: writer.render(), microbit };
}

/**
 * Pins the `.mcprogram` / `.mcprogram.bin` / `.ticks.trace` golden for a draw
 * actuator: the JSON freezes the brain's generated page id, the bytes are
 * byte-stable across builds, two fresh runs render identical traces, and the
 * rendered trace matches the committed golden.
 */
function runDrawFixture(name: string, actuatorName: string, source: string, tickCount: number): string {
  const jsonPath = fileURLToPath(new URL(`./__fixtures__/${name}.mcprogram`, import.meta.url));
  const binPath = fileURLToPath(new URL(`./__fixtures__/${name}.mcprogram.bin`, import.meta.url));
  const tracePath = fileURLToPath(new URL(`./__fixtures__/${name}.ticks.trace`, import.meta.url));

  ensureJsonGolden(jsonPath, actuatorName, source);
  const generated = wodalProgramBytes(new Uint8Array(readFileSync(jsonPath)));
  if (shouldWriteGolden(binPath)) {
    writeFileSync(binPath, generated);
  }
  const bin = new Uint8Array(readFileSync(binPath));
  assert.deepEqual(bin, generated, `${name}.mcprogram.bin is not byte-stable`);

  const first = runTrace(bin, tickCount);
  const second = runTrace(bin, tickCount);
  assert.equal(second.trace, first.trace, "two fresh runs must render byte-identical traces");

  if (shouldWriteGolden(tracePath)) {
    writeFileSync(tracePath, first.trace);
  }
  assert.equal(readFileSync(tracePath, "utf8"), first.trace, `${name}.ticks.trace is not byte-stable`);
  return first.trace;
}

test("a user-tile timed drawImage holds the display, parks, and resumes", () => {
  // 100ms thinks: the 250ms hold (dispatched at think 1, time 100) completes at
  // 350, is resolved by the think-4 poll, and the actuator resumes on think 5.
  const resumeTick = Math.floor((TICK_ADVANCE_MS + HOLD_SECONDS * 1000) / TICK_ADVANCE_MS) + 2;
  const trace = runDrawFixture(
    "user-tile-draw-timed",
    "user-draw-timed",
    actuatorSource("user-draw-timed", `${HOLD_SECONDS}`),
    resumeTick
  );
  const result = runTrace(
    new Uint8Array(
      readFileSync(fileURLToPath(new URL("./__fixtures__/user-tile-draw-timed.mcprogram.bin", import.meta.url)))
    ),
    resumeTick
  );
  const lines = trace.split("\n");
  // One paste at dispatch; the marker pixel only after the hold resolves.
  assert.equal(lines.filter((line) => line === `port display draw 5 5 ${TOP_ROW_HEX}`).length, 1);
  assert.equal(lines.filter((line) => line.startsWith("port display set-pixel ")).length, 1);
  // The async draw is a host function (op 41), so it carries no `action ... async` line.
  assert.equal(lines.filter((line) => line.startsWith("action ") && line.endsWith(" async")).length, 0);
  assert.equal(result.microbit.display.getPixelValue(4, 4), 255);
  assert.equal(lines.filter((line) => line.startsWith("fault ")).length, 0);
});

test("a user-tile fire-and-forget drawImage paints and continues in the same think", () => {
  const trace = runDrawFixture("user-tile-draw-forget", "user-draw-forget", actuatorSource("user-draw-forget", "0"), 2);
  const result = runTrace(
    new Uint8Array(
      readFileSync(fileURLToPath(new URL("./__fixtures__/user-tile-draw-forget.mcprogram.bin", import.meta.url)))
    ),
    2
  );
  const lines = trace.split("\n");
  // The draw and the marker pixel both land on the same think (no park).
  assert.equal(lines.filter((line) => line === `port display draw 5 5 ${TOP_ROW_HEX}`).length, 1);
  assert.equal(lines.filter((line) => line.startsWith("port display set-pixel ")).length, 1);
  // The async draw is a host function (op 41), so it carries no `action ... async` line.
  assert.equal(lines.filter((line) => line.startsWith("action ") && line.endsWith(" async")).length, 0);
  assert.equal(result.microbit.display.getPixelValue(4, 4), 255);
  assert.equal(result.microbit.display.getPixelValue(0, 0), 255);
  assert.equal(lines.filter((line) => line.startsWith("fault ")).length, 0);
});

test("a user-tile that imports the heart icon from the stdlib draws its pixels", () => {
  const trace = runDrawFixture("user-tile-draw-icon", "user-draw-icon", iconActuatorSource("user-draw-icon"), 2);
  const lines = trace.split("\n");
  // The heart icon, constructed at module-init by the stdlib parser, pasted once.
  assert.equal(lines.filter((line) => line === `port display draw 5 5 ${HEART_HEX}`).length, 1);
  // The async draw is a host function (op 41), so it carries no `action ... async` line.
  assert.equal(lines.filter((line) => line.startsWith("action ") && line.endsWith(" async")).length, 0);
  assert.equal(lines.filter((line) => line.startsWith("fault ")).length, 0);
});

test("a user-tile can import and call the stdlib image() builder directly", () => {
  const environment = createMicroBitV2Environment();
  const project = new UserTileProject({
    projectNamespace: TEST_PROJECT_NAMESPACE,
    ambientFiles: wodalAmbientFiles(),
    dependencies: wodalStdlibDependencies(),
    dependencyMounts: wodalStdlibDependencyMounts(),
    services: environment.brainServices,
  });
  const source = `import { Actuator, type Context } from "wendoo";
import { image, heart } from "@lib/wendoo-lang/lib-microbit-v2";

const dot = image(\`
. . . . .
. . f . .
. . . . .
\`);

export default Actuator({
  name: "user-draw-image-builder",
  async onExecute(ctx: Context): Promise<void> {
    await ctx.microbit.display.drawImage(dot, { duration: 0 });
    await ctx.microbit.display.drawImage(heart(), { duration: 0 });
  },
});
`;
  project.setFiles(new Map([["user-draw-image-builder.ts", source]]));
  const compileResult = project.compileAll();
  assert.equal(
    compileResult.tsErrors.size,
    0,
    `Unexpected TypeScript diagnostics: ${JSON.stringify([...compileResult.tsErrors])}`
  );
  for (const [path, result] of compileResult.results) {
    assert.deepEqual(result.diagnostics, [], `Unexpected compiler diagnostics for ${path}`);
    assert.ok(result.program, `Expected a compiled program for ${path}`);
  }
});

test("drawImage's options argument is optional: omitting it typechecks and compiles", () => {
  const environment = createMicroBitV2Environment();
  const project = new UserTileProject({
    projectNamespace: TEST_PROJECT_NAMESPACE,
    ambientFiles: wodalAmbientFiles(),
    services: environment.brainServices,
  });
  const source = `import { Actuator, type Context, type Image } from "wendoo";

export default Actuator({
  name: "user-draw-default-duration",
  async onExecute(ctx: Context): Promise<void> {
    const image: Image = { width: 1, height: 1, pixels: Buffer.from([255]) };
    await ctx.microbit.display.drawImage(image);
  },
});
`;
  project.setFiles(new Map([["user-draw-default-duration.ts", source]]));
  const compileResult = project.compileAll();
  assert.equal(
    compileResult.tsErrors.size,
    0,
    `Unexpected TypeScript diagnostics: ${JSON.stringify([...compileResult.tsErrors])}`
  );
  for (const [path, result] of compileResult.results) {
    assert.deepEqual(result.diagnostics, [], `Unexpected compiler diagnostics for ${path}`);
    assert.ok(result.program, `Expected a compiled program for ${path}`);
  }
});
