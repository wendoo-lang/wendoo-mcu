/**
 * Observable traces for the TS user-code tone API
 * (`ctx.microbit.audio.playTone`): user-tile brains whose compiled async
 * actuators await the asynchronous `ctx.microbit.*` host function (op 41
 * `HOST_CALL_ASYNC`) over the same speaker facade the `beep` tile action
 * drives.
 *
 * Each fixture brain is built through the tile API from compiled user tiles:
 * every root rule fires once on page entry (the core `on page entered`
 * sensor), its DO is one compiled actuator awaiting a tone, and the actuator
 * lights pixel (4,4) once the await resolves, surfacing the resume round. The
 * traces pin the port command the tone crosses the speaker port with -- the
 * waveform word, the clamped pitch, the whole-millisecond duration, and the
 * clamped volume -- plus the lease outcome. Nil and non-finite arguments are
 * exercised directly against the registered host function.
 *
 * The fixture on the cross-VM conformance path pins its `.mcprogram` /
 * `.mcprogram.bin` / `.ticks.trace` triple beside this spec and runs from the
 * committed binary: the C++ VM parity test (cpp/test/trace-parity.test.cpp)
 * loads the same binary, replays the same tick schedule, and byte-compares the
 * trace.
 */

import assert from "node:assert/strict";
import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { test } from "node:test";
import { fileURLToPath } from "node:url";
import {
  type AsyncHandle,
  CoreHostActions,
  type ExecutionContext,
  List,
  mkClosedStructValue,
  mkNativeStructValue,
  mkNumberValue,
  mkSensorTileId,
  mkStringValue,
  NIL_VALUE,
  type Value,
} from "@wendoo/core/app";
import type { IBrainTileDef } from "@wendoo/core/brain";
import { BrainDef, type BrainPageDef, type BrainRuleDef } from "@wendoo/core/brain/model";
import { type LinkedBrainProgram, linkedBrainProgramToJson } from "@wendoo/core/runtime";
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
import { MAX_TONE_FREQUENCY_HZ, type SpeakerToneCommand, type SpeakerToneWaveform } from "../microbit-speaker";
import { createMicroBitV2Environment } from "./environment";
import { WODAL_MICROBIT_V2_TYPE_IDS } from "./module";
import { ObservableTraceWriter, observableTraceVmEvents } from "./observable-trace";
import { WodalMicroBitRuntime } from "./runtime";

/** Milliseconds advanced per scheduled think. */
const TICK_ADVANCE_MS = 100;

/** Milliseconds a tone sounds for when the call omits the duration. */
const DEFAULT_DURATION_MS = 500;

/** Pitch in Hz a tone sounds at when the call omits the frequency. */
const DEFAULT_FREQUENCY_HZ = 880;

/** Wave shape a tone sounds with when the call omits the `waveform` option. */
const DEFAULT_WAVEFORM: SpeakerToneWaveform = "triangle";

/** Canonical `<owner>/<repo>` coordinate the micro:bit v2 standard library is mounted under. */
const MICROBIT_V2_LIB_COORDINATE = "wendoo-lang/lib-microbit-v2";

/** The optional arguments of a `playTone` call; an absent one is left out of the source. */
interface ToneCallSpec {
  /** Anonymous pitch argument in Hz. */
  readonly frequencyHz?: number;

  /** `duration` option in seconds. */
  readonly duration?: number;

  /** `volume` option as a 0-1 fraction. */
  readonly volume?: number;

  /** `waveform` option; any string, including one outside the sounded set. */
  readonly waveform?: string;

  /** `waveform` option named through the standard library's `waveforms` const object. */
  readonly waveformName?: SpeakerToneWaveform;

  /** `immediately` option: preempt the speaker lease at dispatch. */
  readonly immediately?: boolean;

  /** `inBackground` option: resolve the call at dispatch. */
  readonly inBackground?: boolean;
}

/**
 * A `ctx.microbit.audio.playTone(...)` call, adding the options bag only when an
 * option is set. A spec that sets any option must also set `frequencyHz`.
 */
function playToneCall(spec: ToneCallSpec): string {
  const entries: string[] = [];
  if (spec.duration !== undefined) {
    entries.push(`duration: ${spec.duration}`);
  }
  if (spec.volume !== undefined) {
    entries.push(`volume: ${spec.volume}`);
  }
  if (spec.waveform !== undefined) {
    entries.push(`waveform: ${JSON.stringify(spec.waveform)}`);
  }
  if (spec.waveformName !== undefined) {
    entries.push(`waveform: waveforms.${spec.waveformName}`);
  }
  if (spec.immediately) {
    entries.push("immediately: true");
  }
  if (spec.inBackground) {
    entries.push("inBackground: true");
  }
  const args: string[] = [];
  if (spec.frequencyHz !== undefined) {
    args.push(String(spec.frequencyHz));
  }
  if (entries.length > 0) {
    assert.ok(spec.frequencyHz !== undefined, "a call carrying options must name its frequency");
    args.push(`{ ${entries.join(", ")} }`);
  }
  return `ctx.microbit.audio.playTone(${args.join(", ")})`;
}

/**
 * Source of an async actuator that awaits one `playTone` call and, when
 * `marker` is set, lights pixel (4,4) once the await resolves. A spec naming its
 * wave shape through `waveformName` imports the standard library's `waveforms`
 * const object.
 */
function actuatorSource(name: string, spec: ToneCallSpec, marker: boolean): string {
  const markerLine = marker ? "\n    ctx.microbit.display.setPixelValue(4, 4, 255);" : "";
  const stdlibImport =
    spec.waveformName === undefined ? "" : `import { waveforms } from "@lib/${MICROBIT_V2_LIB_COORDINATE}";\n`;
  return `import { Actuator, type Context } from "wendoo";
${stdlibImport}

export default Actuator({
  name: "${name}",
  async onExecute(ctx: Context): Promise<void> {
    await ${playToneCall(spec)};${markerLine}
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

/** The micro:bit v2 standard library dependency: `@lib/wendoo-lang/lib-microbit-v2` resolves to its entry module. */
function wodalStdlibDependencies(): readonly ProjectDependency[] {
  return [{ coordinate: MICROBIT_V2_LIB_COORDINATE }];
}

/** The micro:bit v2 standard library content, mounted read-only for `@lib/wendoo-lang/lib-microbit-v2` resolution. */
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

/** The compiled actuator tile whose label is the given actuator name. */
function findActuatorTile(tiles: readonly IBrainTileDef[], name: string): IBrainTileDef {
  const tile = tiles.find((candidate) => candidate.kind === "actuator" && candidate.metadata?.label === name);
  assert.ok(tile, `actuator tile '${name}' should be in the bundle`);
  return tile;
}

/** One rule of a fixture brain: the call its compiled actuator awaits. */
interface ToneRuleSpec extends ToneCallSpec {
  /** Light pixel (4,4) once the await resolves, marking the resume round. */
  readonly marker?: boolean;
}

/**
 * Compiles one actuator per rule and builds a single-page brain running them.
 * With `structure: "chain"` (the default) each further actuator runs in a child
 * rule nested under the previous one, firing when its parent's do completes;
 * with `structure: "siblings"` each runs in its own root rule, so all dispatch
 * the same round and compete for the speaker lease.
 */
function buildImage(
  rules: readonly ToneRuleSpec[],
  structure: "chain" | "siblings" = "chain"
): WodalProgramImage<LinkedBrainProgram> {
  const environment = createMicroBitV2Environment();
  const files = new Map<string, string>();
  const actuatorNames = rules.map((_spec, index) => `user-play-tone-${index}`);
  for (let i = 0; i < rules.length; i++) {
    files.set(`${actuatorNames[i]}.ts`, actuatorSource(actuatorNames[i]!, rules[i]!, rules[i]!.marker === true));
  }

  const project = new UserTileProject({
    projectNamespace: TEST_PROJECT_NAMESPACE,
    ambientFiles: wodalAmbientFiles(),
    dependencies: wodalStdlibDependencies(),
    dependencyMounts: wodalStdlibDependencyMounts(),
    services: environment.brainServices,
  });
  project.setFiles(files);
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

  const brainDef = BrainDef.emptyBrainDef(environment.brainServices, "user-tile play-tone brain");
  const page = brainDef.pages().get(0)! as BrainPageDef;
  let rule = page.children().get(0)! as BrainRuleDef;
  rule.when().appendTile(onPageEntered);
  for (let i = 0; i < actuatorNames.length; i++) {
    if (i > 0) {
      rule = structure === "siblings" ? (page.appendNewRule() as BrainRuleDef) : rule.appendNewRule();
      // A sibling root fires on page entry like the first; a chained child
      // fires when its parent's do completes, so it carries no when tile.
      if (structure === "siblings") {
        rule.when().appendTile(onPageEntered);
      }
    }
    rule.do().appendTile(findActuatorTile(bundle.tiles, actuatorNames[i]!));
  }

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

/**
 * Runs `image` over `tickCount` thinks at {@link TICK_ADVANCE_MS} each with the
 * trace observers installed: the on-page-entered sensor, the speaker port (a
 * tone the busy speaker drops, and one with a negative duration, cross no port
 * and emit no line), the set-pixel port, and fiber faults.
 */
function runTrace(image: WodalProgramImage<LinkedBrainProgram>, tickCount: number): string {
  const environment = createMicroBitV2Environment();
  const profile = getWodalDeviceProfile(WodalDeviceProfileId.MICROBIT_V2);
  const writer = new ObservableTraceWriter({
    profileId: profile.numericProfileId,
    precision: profile.numberPrecision,
  });

  const microbit = new MicroBit();
  const devicePlayTone = microbit.speaker.playTone.bind(microbit.speaker);
  microbit.speaker.playTone = (command, requestTime, onEnd) => {
    if (!microbit.speaker.isBusy() && command.durationMs >= 0) {
      writer.speakerTone(command);
    }
    devicePlayTone(command, requestTime, onEnd);
  };
  const deviceSetPixelValue = microbit.display.setPixelValue.bind(microbit.display);
  microbit.display.setPixelValue = (x, y, brightness) => {
    writer.displaySetPixel(x, y, brightness);
    deviceSetPixelValue(x, y, brightness);
  };

  const runtime = new WodalMicroBitRuntime({ environment, microbit, vmEvents: observableTraceVmEvents(writer) });
  assert.deepEqual(runtime.loadWodalProgramImage(image), { ok: true });

  for (let i = 0; i < tickCount; i++) {
    const timeMs = (i + 1) * TICK_ADVANCE_MS;
    writer.tick(i + 1, timeMs, i === 0 ? 0 : TICK_ADVANCE_MS);
    runtime.tick(TICK_ADVANCE_MS);
  }
  return writer.render();
}

/** Decodes a committed fixture binary into a loadable microbit-v2 program image. */
function imageFromBytes(bin: Uint8Array): WodalProgramImage<LinkedBrainProgram> {
  const environment = createMicroBitV2Environment();
  const profile = getWodalDeviceProfile(WodalDeviceProfileId.MICROBIT_V2);
  const decoded = parseWodalProgramImageBytes(
    bin,
    WodalDeviceProfileId.MICROBIT_V2,
    environment.brainServices.runtime.types
  );
  return profile.createProgramImage(decoded.program);
}

/** Checks a rendered trace covers the whole schedule, faults nowhere, and dispatches one actuator per rule. */
function assertToneTrace(trace: string, tickCount: number, dispatchCount: number): void {
  const lines = trace.split("\n");
  assert.equal(lines.filter((line) => line.startsWith("tick ")).length, tickCount);
  assert.equal(lines.filter((line) => line.startsWith("fault ")).length, 0);
  // Each compiled async actuator dispatches once; the host function (op 41) it
  // awaits carries no `action ... async` line of its own.
  assert.equal(lines.filter((line) => line.startsWith("tile ") && line.endsWith(" async")).length, dispatchCount);
  assert.equal(lines.filter((line) => line.startsWith("action ") && line.endsWith(" async")).length, 0);
}

/** Builds and runs a fixture, checking it is fault-free and reproducible. */
function runToneFixture(
  rules: readonly ToneRuleSpec[],
  tickCount: number,
  structure: "chain" | "siblings" = "chain"
): string {
  const image = buildImage(rules, structure);
  const first = runTrace(image, tickCount);
  assert.equal(runTrace(image, tickCount), first, "two fresh runs must render byte-identical traces");
  assertToneTrace(first, tickCount, rules.length);
  return first;
}

/**
 * Pins the `.mcprogram` / `.mcprogram.bin` / `.ticks.trace` golden triple of the
 * cross-VM tone fixture and returns the rendered trace: the JSON freezes the
 * brain's generated page id, the bytes are byte-stable across builds, the trace
 * is rendered from the committed binary the C++ parity test loads, two fresh
 * runs render identical traces, and the rendered trace matches the committed
 * golden.
 */
function runToneGoldenFixture(
  name: string,
  rules: readonly ToneRuleSpec[],
  tickCount: number,
  structure: "chain" | "siblings" = "chain"
): string {
  const jsonPath = fileURLToPath(new URL(`./__fixtures__/${name}.mcprogram`, import.meta.url));
  const binPath = fileURLToPath(new URL(`./__fixtures__/${name}.mcprogram.bin`, import.meta.url));
  const tracePath = fileURLToPath(new URL(`./__fixtures__/${name}.ticks.trace`, import.meta.url));

  if (!existsSync(jsonPath)) {
    const image = buildImage(rules, structure);
    writeFileSync(
      jsonPath,
      serializeWodalProgramImageJson({ ...image, program: linkedBrainProgramToJson(image.program) })
    );
  }
  const generated = wodalProgramBytes(new Uint8Array(readFileSync(jsonPath)));
  if (shouldWriteGolden(binPath)) {
    writeFileSync(binPath, generated);
  }
  const bin = new Uint8Array(readFileSync(binPath));
  assert.deepEqual(bin, generated, `${name}.mcprogram.bin is not byte-stable`);

  const first = runTrace(imageFromBytes(bin), tickCount);
  const second = runTrace(imageFromBytes(bin), tickCount);
  assert.equal(second, first, "two fresh runs must render byte-identical traces");
  assertToneTrace(first, tickCount, rules.length);

  if (shouldWriteGolden(tracePath)) {
    writeFileSync(tracePath, first);
  }
  assert.equal(readFileSync(tracePath, "utf8"), first, `${name}.ticks.trace is not byte-stable`);
  return first;
}

/** The f32 bit pattern of `value` as the trace renders a brain-observable number. */
function f32Bits(value: number): string {
  const view = new DataView(new ArrayBuffer(4));
  view.setFloat32(0, value);
  return view.getUint32(0).toString(16).padStart(8, "0");
}

/** The `port speaker tone` line a tone crossing the port with these values renders. */
function toneLine(waveform: SpeakerToneWaveform, frequencyHz: number, durationMs: number, volume: number): string {
  return `port speaker tone ${waveform} ${f32Bits(frequencyHz)} ${(durationMs >>> 0).toString(16)} ${f32Bits(volume)}`;
}

/**
 * The 1-based tick on which a compiled actuator's tone crosses the speaker
 * port. Its rule fires on tick 1 and the async host call runs on the next
 * think.
 */
const DISPATCH_TICK = 2;

/**
 * The 1-based tick on which an awaited tone dispatched by a rule that fired on
 * tick 1 resumes its actuator. The lease starts at the rule's round time and
 * settles on the first think at or past its end (after that think ran), and the
 * fiber resumes on the think after that.
 */
function resumeTickFor(durationMs: number): number {
  return 1 + Math.max(1, Math.ceil(durationMs / TICK_ADVANCE_MS)) + 1;
}

/** Ticks enough for a chain of awaited tones of the given durations to run out. */
function chainTickCount(durationsMs: readonly number[]): number {
  let ruleTick = 1;
  for (const durationMs of durationsMs) {
    ruleTick = resumeTickFor(durationMs) + ruleTick;
  }
  return ruleTick;
}

/** The 1-based tick index whose block contains the first line matching `predicate`, or -1. */
function tickOfLine(trace: string, predicate: (line: string) => boolean): number {
  let currentTick = 0;
  for (const line of trace.split("\n")) {
    if (line.startsWith("tick ")) {
      currentTick = Number.parseInt(line.split(" ")[1]!, 16);
    } else if (predicate(line)) {
      return currentTick;
    }
  }
  return -1;
}

/** The 1-based tick index of the last line matching `predicate`, or -1. */
function lastTickOfLine(trace: string, predicate: (line: string) => boolean): number {
  let currentTick = 0;
  let found = -1;
  for (const line of trace.split("\n")) {
    if (line.startsWith("tick ")) {
      currentTick = Number.parseInt(line.split(" ")[1]!, 16);
    } else if (predicate(line)) {
      found = currentTick;
    }
  }
  return found;
}

/** Count of lines in `trace` exactly equal to `line`. */
function countLines(trace: string, line: string): number {
  return trace.split("\n").filter((candidate) => candidate === line).length;
}

/** Count of `port speaker tone` lines in `trace`. */
function countToneLines(trace: string): number {
  return trace.split("\n").filter((line) => line.startsWith("port speaker tone ")).length;
}

/** Count of `port display set-pixel` lines in `trace`. */
function countMarkers(trace: string): number {
  return trace.split("\n").filter((line) => line.startsWith("port display set-pixel ")).length;
}

test("a user-tile awaited playTone holds the speaker for the default tone, parks, and resumes", () => {
  const markerTick = resumeTickFor(DEFAULT_DURATION_MS);
  const trace = runToneFixture([{ marker: true }], markerTick);
  assert.equal(countToneLines(trace), 1);
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, DEFAULT_FREQUENCY_HZ, DEFAULT_DURATION_MS, 1)), 1);
  assert.equal(
    tickOfLine(trace, (l) => l.startsWith("port speaker tone ")),
    DISPATCH_TICK
  );
  assert.equal(countMarkers(trace), 1);
  assert.equal(
    tickOfLine(trace, (l) => l.startsWith("port display set-pixel ")),
    markerTick
  );
});

test("a chained pair of playTone calls sounds the default tone, then one carrying every option", () => {
  // The first rule awaits a bare call (every default); its child rule awaits a
  // call carrying the frequency and every option, so the waveform name, the
  // duration, and the volume all cross the port through the host-function path.
  const firstResolveTick = resumeTickFor(DEFAULT_DURATION_MS);
  const secondRuleTick = firstResolveTick + 1;
  const markerTick = secondRuleTick - 1 + resumeTickFor(300);
  const trace = runToneGoldenFixture(
    "user-tile-play-tone",
    [{}, { frequencyHz: 262, duration: 0.3, volume: 0.5, waveform: "square", marker: true }],
    markerTick
  );
  assert.equal(countToneLines(trace), 2);
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, DEFAULT_FREQUENCY_HZ, DEFAULT_DURATION_MS, 1)), 1);
  assert.equal(countLines(trace, toneLine("square", 262, 300, 0.5)), 1);
  // The second call dispatches on the think after its rule fired, which is one
  // think after the first tone's await resolved.
  assert.equal(
    tickOfLine(trace, (l) => l.startsWith("port speaker tone ")),
    DISPATCH_TICK
  );
  assert.equal(
    lastTickOfLine(trace, (l) => l.startsWith("port speaker tone ")),
    secondRuleTick + 1
  );
  assert.equal(countMarkers(trace), 1);
  assert.equal(
    tickOfLine(trace, (l) => l.startsWith("port display set-pixel ")),
    markerTick
  );
});

test("each playTone argument and option sets its field of the port command", () => {
  const waveforms: readonly SpeakerToneWaveform[] = ["square", "sawtooth", "sine", "triangle"];
  const rules: ToneRuleSpec[] = [
    { frequencyHz: 440 },
    { frequencyHz: DEFAULT_FREQUENCY_HZ, duration: 0.2 },
    { frequencyHz: DEFAULT_FREQUENCY_HZ, volume: 0.25 },
    ...waveforms.map((waveform) => ({ frequencyHz: DEFAULT_FREQUENCY_HZ, waveform })),
  ];
  const durations = [DEFAULT_DURATION_MS, 200, DEFAULT_DURATION_MS, ...waveforms.map(() => DEFAULT_DURATION_MS)];
  const trace = runToneFixture(rules, chainTickCount(durations));

  assert.equal(countToneLines(trace), rules.length);
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, 440, DEFAULT_DURATION_MS, 1)), 1);
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, DEFAULT_FREQUENCY_HZ, 200, 1)), 1);
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, DEFAULT_FREQUENCY_HZ, DEFAULT_DURATION_MS, 0.25)), 1);
  for (const waveform of waveforms) {
    assert.equal(countLines(trace, toneLine(waveform, DEFAULT_FREQUENCY_HZ, DEFAULT_DURATION_MS, 1)), 1);
  }
});

test("a playTone pitch and volume cross the port clamped, with 0 Hz encoded as a silent rest", () => {
  const rules: ToneRuleSpec[] = [
    { frequencyHz: 0, volume: 0.5, duration: 0.1 },
    { frequencyHz: -5, duration: 0.2 },
    { frequencyHz: 20000, duration: 0.3 },
    { frequencyHz: DEFAULT_FREQUENCY_HZ, volume: 2, duration: 0.4 },
    { frequencyHz: DEFAULT_FREQUENCY_HZ, volume: -1, duration: 0.5 },
  ];
  const trace = runToneFixture(rules, chainTickCount([100, 200, 300, 400, 500]));

  assert.equal(countToneLines(trace), rules.length);
  // A rest encodes at volume 0 whatever volume was asked for, and a negative
  // pitch clamps onto the same rest.
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, 0, 100, 0)), 1);
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, 0, 200, 0)), 1);
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, MAX_TONE_FREQUENCY_HZ, 300, 1)), 1);
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, DEFAULT_FREQUENCY_HZ, 400, 1)), 1);
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, DEFAULT_FREQUENCY_HZ, 500, 0)), 1);
});

test("a playTone naming its waveform through the standard library const sounds that wave shape", () => {
  // Each member of the stdlib `waveforms` const object reaches the port as the
  // wave shape it names, pinning its value against the set the port sounds.
  const waveformNames: readonly SpeakerToneWaveform[] = ["square", "sawtooth", "sine", "triangle"];
  const rules: ToneRuleSpec[] = waveformNames.map((waveformName) => ({
    frequencyHz: DEFAULT_FREQUENCY_HZ,
    waveformName,
  }));
  const trace = runToneFixture(rules, chainTickCount(waveformNames.map(() => DEFAULT_DURATION_MS)));

  assert.equal(countToneLines(trace), waveformNames.length);
  for (const waveform of waveformNames) {
    assert.equal(countLines(trace, toneLine(waveform, DEFAULT_FREQUENCY_HZ, DEFAULT_DURATION_MS, 1)), 1);
  }
});

test("a playTone naming a waveform outside the sounded set is a silent no-op that resolves at once", () => {
  const trace = runToneFixture([{ frequencyHz: 440, waveform: "bogus", marker: true }], 3);
  assert.equal(countToneLines(trace), 0);
  assert.equal(countMarkers(trace), 1);
  assert.equal(
    tickOfLine(trace, (l) => l.startsWith("port display set-pixel ")),
    DISPATCH_TICK
  );
});

test("a playTone with a negative duration sounds nothing, crosses no port, and resolves at dispatch", () => {
  const trace = runToneFixture([{ frequencyHz: 440, duration: -1, marker: true }], 3);
  assert.equal(countToneLines(trace), 0);
  assert.equal(
    tickOfLine(trace, (l) => l.startsWith("port display set-pixel ")),
    DISPATCH_TICK
  );
});

test("a playTone dispatched while the speaker is busy is silently dropped", () => {
  const markerTick = resumeTickFor(DEFAULT_DURATION_MS);
  const trace = runToneFixture([{ marker: true }, { frequencyHz: 440, marker: true }], markerTick, "siblings");
  // Only the holder's tone crosses the port; the competitor's is dropped.
  assert.equal(countToneLines(trace), 1);
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, DEFAULT_FREQUENCY_HZ, DEFAULT_DURATION_MS, 1)), 1);
  // The dropped call resolves at dispatch; the holder resumes after its duration.
  assert.equal(countMarkers(trace), 2);
  assert.equal(
    tickOfLine(trace, (l) => l.startsWith("port display set-pixel ")),
    DISPATCH_TICK
  );
  assert.equal(
    lastTickOfLine(trace, (l) => l.startsWith("port display set-pixel ")),
    markerTick
  );
});

test("a playTone with immediately preempts the holder, whose await resolves", () => {
  const markerTick = resumeTickFor(DEFAULT_DURATION_MS);
  const holderNaturalResume = resumeTickFor(1000);
  const trace = runToneFixture(
    [
      { frequencyHz: 440, duration: 1, marker: true },
      { frequencyHz: DEFAULT_FREQUENCY_HZ, immediately: true, marker: true },
    ],
    markerTick,
    "siblings"
  );
  // Both tones cross the port on the dispatch tick: the holder's, then the
  // preemptor's after the preempt released the lease.
  assert.equal(countToneLines(trace), 2);
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, 440, 1000, 1)), 1);
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, DEFAULT_FREQUENCY_HZ, DEFAULT_DURATION_MS, 1)), 1);
  assert.equal(
    lastTickOfLine(trace, (l) => l.startsWith("port speaker tone ")),
    DISPATCH_TICK
  );
  // The preempted holder resumes well before its own tone would have ended.
  const firstMarkerTick = tickOfLine(trace, (l) => l.startsWith("port display set-pixel "));
  assert.ok(firstMarkerTick > 0 && firstMarkerTick < holderNaturalResume);
  assert.equal(
    lastTickOfLine(trace, (l) => l.startsWith("port display set-pixel ")),
    markerTick
  );
});

test("a playTone in background keeps its lease while the caller continues", () => {
  // The lease settles on tick time after the duration; stop short of that.
  const tickCount = resumeTickFor(DEFAULT_DURATION_MS) - 2;
  const trace = runToneFixture([{ frequencyHz: DEFAULT_FREQUENCY_HZ, inBackground: true, marker: true }], tickCount);
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, DEFAULT_FREQUENCY_HZ, DEFAULT_DURATION_MS, 1)), 1);
  // The handle resolves at dispatch: the marker lands on the dispatch tick while
  // the tone still holds the speaker lease for several more thinks.
  assert.equal(countMarkers(trace), 1);
  assert.equal(
    tickOfLine(trace, (l) => l.startsWith("port display set-pixel ")),
    DISPATCH_TICK
  );
  assert.ok(tickCount < resumeTickFor(DEFAULT_DURATION_MS), "the background tone still holds the lease at the end");
});

test("playTone options combine with a lease flag on one call", () => {
  const tickCount = resumeTickFor(300) - 1;
  const trace = runToneFixture(
    [{ frequencyHz: 262, duration: 0.3, volume: 0.5, waveform: "square", inBackground: true, marker: true }],
    tickCount
  );
  assert.equal(countToneLines(trace), 1);
  assert.equal(countLines(trace, toneLine("square", 262, 300, 0.5)), 1);
  assert.equal(
    tickOfLine(trace, (l) => l.startsWith("port display set-pixel ")),
    DISPATCH_TICK
  );
});

test("a nil or non-finite playTone argument reads as its default", () => {
  assert.deepEqual(dispatchedTone({}), {
    waveform: DEFAULT_WAVEFORM,
    frequencyHz: DEFAULT_FREQUENCY_HZ,
    durationMs: DEFAULT_DURATION_MS,
    volume: 1,
  });
  const defaults: SpeakerToneCommand = {
    waveform: DEFAULT_WAVEFORM,
    frequencyHz: DEFAULT_FREQUENCY_HZ,
    durationMs: DEFAULT_DURATION_MS,
    volume: 1,
  };
  for (const value of [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY]) {
    assert.deepEqual(dispatchedTone({ frequencyHz: value }), defaults);
    assert.deepEqual(dispatchedTone({ duration: value }), defaults);
    assert.deepEqual(dispatchedTone({ volume: value }), defaults);
  }
});

/** The `playTone` arguments a direct dispatch fills; an absent one is nil. */
interface ToneArgs {
  /** Anonymous pitch argument in Hz. */
  readonly frequencyHz?: number;

  /** `duration` option field in seconds. */
  readonly duration?: number;

  /** `volume` option field as a 0-1 fraction. */
  readonly volume?: number;

  /** `waveform` option field. */
  readonly waveform?: string;
}

/**
 * Dispatches the `MicroBitAudio.playTone` host function directly with `args`
 * and returns the command it hands the speaker port, or undefined when it hands
 * the port none. Accepts nil and non-finite values.
 */
function dispatchedTone(args: ToneArgs): SpeakerToneCommand | undefined {
  const environment = createMicroBitV2Environment();
  const microbit = new MicroBit();
  const entry = environment.brainServices.runtime.functions.get("MicroBitAudio.playTone");
  assert.ok(entry, "the playTone host function should be registered");
  assert.equal(entry.isAsync, true);

  const options = mkClosedStructValue(
    WODAL_MICROBIT_V2_TYPE_IDS.PlayToneOptions,
    List.from([
      numberOrNil(args.duration),
      numberOrNil(args.volume),
      args.waveform === undefined ? NIL_VALUE : mkStringValue(args.waveform),
      NIL_VALUE,
      NIL_VALUE,
    ])
  );
  const callArgs = List.from<Value>([
    mkNativeStructValue(WODAL_MICROBIT_V2_TYPE_IDS.MicroBitAudio, microbit.speaker),
    numberOrNil(args.frequencyHz),
    options,
  ]);

  let dispatched: SpeakerToneCommand | undefined;
  const devicePlayTone = microbit.speaker.playTone.bind(microbit.speaker);
  microbit.speaker.playTone = (command, requestTime, onEnd) => {
    dispatched = command;
    devicePlayTone(command, requestTime, onEnd);
  };

  const handle: AsyncHandle = { id: 1, resolve: () => {}, reject: () => {}, cancel: () => {} };
  // The host function reads only the dispatch time from its context.
  entry.fn.exec({ time: 0 } as unknown as ExecutionContext, callArgs, handle);
  return dispatched;
}

/** `value` as a Number value, or nil when it is absent. */
function numberOrNil(value: number | undefined): Value {
  return value === undefined ? NIL_VALUE : mkNumberValue(value);
}
