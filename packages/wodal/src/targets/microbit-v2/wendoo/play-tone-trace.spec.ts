/**
 * Observable traces for REAL compiled brains exercising the `beep` actuator
 * against the speaker lease. Each fixture brain is built through the tile API
 * and compiled by the brain compiler: a root rule fires once on page entry (the
 * core `on page entered` sensor), its DO is one tone dispatch, and a child rule
 * lights a pixel once the parent's DO completes (the compiler-emitted
 * SPAWN_RULE), surfacing the resume round. A `siblings` fixture gives every
 * spec its own root rule, so all dispatch the same round and compete for the
 * speaker; a `chain` fixture nests each spec under the previous one, so each
 * tone dispatches on the round the tone before it released the lease.
 *
 * The traces pin the port command the tone crosses the speaker port with -- the
 * waveform word, the clamped pitch, the whole-millisecond duration, and the
 * clamped volume -- plus the lease outcome. Non-finite arguments and the
 * speaker's own state are exercised directly against the actuator and the port.
 *
 * The fixtures on the cross-VM conformance path pin their `.mcprogram` /
 * `.mcprogram.bin` / `.ticks.trace` triple beside this spec and run from the
 * committed binary: the C++ VM parity test (cpp/test/trace-parity.test.cpp)
 * loads the same binaries, replays the same tick schedule, and byte-compares
 * the traces.
 */

import assert from "node:assert/strict";
import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { test } from "node:test";
import { fileURLToPath } from "node:url";
import {
  type AsyncHandle,
  BrainTileLiteralDef,
  CoreHostActions,
  CoreParameterId,
  CoreTypeIds,
  type ExecutionContext,
  getSlotId,
  List,
  mkActuatorTileId,
  mkModifierTileId,
  mkNumberValue,
  mkParameterTileId,
  mkSensorTileId,
  NIL_VALUE,
  type Value,
  type WendooEnvironment,
} from "@wendoo/core/app";
import { BrainDef, type BrainPageDef, type BrainRuleDef } from "@wendoo/core/brain/model";
import { type LinkedBrainProgram, linkedBrainProgramToJson } from "@wendoo/core/runtime";
import { buildWodalProgramImage } from "../../../wendoo/build-kernel";
import { getWodalDeviceProfile, WodalDeviceProfileId } from "../../../wendoo/device-profile";
import { shouldWriteGolden } from "../../../wendoo/golden-regeneration";
import { serializeWodalProgramImageJson, type WodalProgramImage } from "../../../wendoo/program-image";
import { parseWodalProgramImageBytes, wodalProgramBytes } from "../../../wendoo/program-image-binary";
import { MicroBit } from "../microbit";
import {
  MAX_TONE_FREQUENCY_HZ,
  MicroBitSpeaker,
  mkSpeakerToneCommand,
  type SpeakerToneCommand,
  type SpeakerToneWaveform,
} from "../microbit-speaker";
import { createMicroBitV2Environment } from "./environment";
import { ObservableTraceWriter, observableTraceVmEvents } from "./observable-trace";
import { WodalMicroBitRuntime } from "./runtime";
import { MicroBitV2HostActions, WodalMicroBitV2ModifierId, WodalMicroBitV2ParameterId } from "./tile-ids";

/** Hex form of the play-tone action id, as the `action ... async` dispatch line renders it. */
const PLAY_TONE_HEX = (MicroBitV2HostActions.PlayTone.actionId >>> 0).toString(16);

/** Milliseconds advanced per scheduled think. */
const TICK_ADVANCE_MS = 100;

/** Milliseconds a tone sounds for when the call omits the duration. */
const DEFAULT_DURATION_MS = 500;

/** Pitch in Hz a tone sounds at when the call omits the anonymous frequency. */
const DEFAULT_FREQUENCY_HZ = 880;

/** Wave shape a tone sounds with when no wave-shape modifier is attached. */
const DEFAULT_WAVEFORM: SpeakerToneWaveform = "triangle";

/** The wave-shape modifier tile id selecting each waveform word. */
const WAVEFORM_MODIFIER_ID: Readonly<Record<SpeakerToneWaveform, string>> = {
  square: WodalMicroBitV2ModifierId.Square,
  sawtooth: WodalMicroBitV2ModifierId.Sawtooth,
  sine: WodalMicroBitV2ModifierId.Sine,
  triangle: WodalMicroBitV2ModifierId.Triangle,
};

/** One root rule of a fixture brain: the tone arguments and modifiers it carries. */
interface ToneRuleSpec {
  /** Anonymous pitch argument in Hz; absent leaves the slot empty. */
  readonly frequencyHz?: number;

  /** Named duration argument in seconds; absent leaves the slot empty. */
  readonly durationSeconds?: number;

  /** Named volume argument as a 0-1 fraction; absent leaves the slot empty. */
  readonly volume?: number;

  /** Wave-shape modifier to attach; absent attaches none. */
  readonly waveform?: SpeakerToneWaveform;

  /** Attach the `immediately` modifier. */
  readonly immediately?: boolean;

  /** Attach the `in background` modifier. */
  readonly inBackground?: boolean;
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
 * The 1-based tick on which an awaited tone dispatched on tick 1 resumes its
 * rule: the lease settles on the first think at or past `dispatch + duration`
 * (after that think ran), and the fiber resumes on the think after that.
 */
function resumeTickFor(durationMs: number): number {
  return Math.ceil((TICK_ADVANCE_MS + durationMs) / TICK_ADVANCE_MS) + 1;
}

/**
 * How a fixture brain lays its tone rules out.
 *
 * - `siblings`: one root rule per spec, all firing on page entry, so every tone
 *   dispatches the same round and competes for the speaker lease.
 * - `chain`: the first spec is the root rule and each further spec is a child of
 *   the one before it, so a tone dispatches on the round its parent's tone
 *   released the lease.
 */
type BrainStructure = "siblings" | "chain";

/**
 * A one-page brain running the given tone specs in the given layout: each rule's
 * DO is a `beep` carrying the spec's arguments and modifiers, and a child rule
 * lights a pixel once that DO completes (per root rule in `siblings`, once under
 * the last rule of the `chain`).
 */
function buildBrainDef(
  env: WendooEnvironment,
  name: string,
  rules: readonly ToneRuleSpec[],
  structure: BrainStructure
): BrainDef {
  const tiles = env.brainServices.edit.tiles;
  const onPageEntered = tiles.get(mkSensorTileId(CoreHostActions.OnPageEntered.key));
  const beepTile = tiles.get(mkActuatorTileId(MicroBitV2HostActions.PlayTone.key));
  const durationParam = tiles.get(mkParameterTileId(WodalMicroBitV2ParameterId.Duration));
  const volumeParam = tiles.get(mkParameterTileId(WodalMicroBitV2ParameterId.Volume));
  const immediately = tiles.get(mkModifierTileId(WodalMicroBitV2ModifierId.Immediately));
  const inBackground = tiles.get(mkModifierTileId(WodalMicroBitV2ModifierId.InBackground));
  const setPixelTile = tiles.get(mkActuatorTileId(MicroBitV2HostActions.DisplaySetPixel.key));
  assert.ok(onPageEntered);
  assert.ok(beepTile);
  assert.ok(durationParam);
  assert.ok(volumeParam);
  assert.ok(immediately);
  assert.ok(inBackground);
  assert.ok(setPixelTile);

  const brainDef = BrainDef.emptyBrainDef(env.brainServices, `${name} brain`);
  const page = brainDef.pages().get(0)! as BrainPageDef;

  /** Registers a Number literal tile and appends it to `rule`'s DO. */
  const appendNumber = (rule: BrainRuleDef, value: number): void => {
    const literal = new BrainTileLiteralDef(CoreTypeIds.Number, value, {}, env.brainServices);
    brainDef.catalog().registerTileDef(literal);
    rule.do().appendTile(literal);
  };

  let rule = page.children().get(0)! as BrainRuleDef;
  for (let i = 0; i < rules.length; i++) {
    const spec = rules[i]!;
    if (i > 0) {
      rule = structure === "siblings" ? (page.appendNewRule() as BrainRuleDef) : rule.appendNewRule();
    }
    // A sibling root fires on page entry like the first; a chained child fires
    // when its parent's do completes, so it carries no when tile.
    if (structure === "siblings" || i === 0) {
      rule.when().appendTile(onPageEntered);
    }
    rule.do().appendTile(beepTile);
    if (spec.frequencyHz !== undefined) {
      appendNumber(rule, spec.frequencyHz);
    }
    if (spec.durationSeconds !== undefined) {
      rule.do().appendTile(durationParam);
      appendNumber(rule, spec.durationSeconds);
    }
    if (spec.volume !== undefined) {
      rule.do().appendTile(volumeParam);
      appendNumber(rule, spec.volume);
    }
    if (spec.waveform !== undefined) {
      const waveformTile = tiles.get(mkModifierTileId(WAVEFORM_MODIFIER_ID[spec.waveform]));
      assert.ok(waveformTile, `modifier tile for '${spec.waveform}' should be registered`);
      rule.do().appendTile(waveformTile);
    }
    if (spec.immediately) {
      rule.do().appendTile(immediately);
    }
    if (spec.inBackground) {
      rule.do().appendTile(inBackground);
    }
    if (structure === "siblings") {
      rule.appendNewRule().do().appendTile(setPixelTile);
    }
  }
  if (structure === "chain") {
    rule.appendNewRule().do().appendTile(setPixelTile);
  }

  return brainDef;
}

/** Compiles the fixture brain for the microbit-v2 profile. */
function buildImage(
  name: string,
  rules: readonly ToneRuleSpec[],
  structure: BrainStructure
): WodalProgramImage<LinkedBrainProgram> {
  const environment = createMicroBitV2Environment();
  const built = buildWodalProgramImage({
    brainDef: buildBrainDef(environment, name, rules, structure),
    environment,
    deviceProfile: getWodalDeviceProfile(WodalDeviceProfileId.MICROBIT_V2),
  });
  if (!built.ok) {
    assert.fail(`expected a successful build: ${JSON.stringify(built.errors)}`);
  }
  return built.image;
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

/**
 * Runs `image` over `tickCount` thinks at {@link TICK_ADVANCE_MS} each with the
 * trace observers installed: the on-page-entered and set-pixel sync actions, the
 * async play-tone action, the speaker port (a tone the busy speaker drops, or one
 * with a negative duration, crosses no port and emits no line), and fiber faults.
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
    // A dropped tone, and a dropped negative-duration segment, cross no port.
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

  const vmEvents = observableTraceVmEvents(writer);
  const runtime = new WodalMicroBitRuntime({ environment, microbit, vmEvents });
  assert.deepEqual(runtime.loadWodalProgramImage(image), { ok: true });

  let lastThinkTimeMs = 0;
  for (let i = 0; i < tickCount; i++) {
    const timeMs = lastThinkTimeMs + TICK_ADVANCE_MS;
    writer.tick(i + 1, timeMs, lastThinkTimeMs === 0 ? 0 : timeMs - lastThinkTimeMs);
    runtime.tick(TICK_ADVANCE_MS);
    lastThinkTimeMs = timeMs;
  }
  return writer.render();
}

/** Checks a rendered trace covers the whole schedule, faults nowhere, and dispatches one tone per rule. */
function assertToneTrace(trace: string, tickCount: number, dispatchCount: number): void {
  const lines = trace.split("\n");
  assert.equal(lines.filter((line) => line.startsWith("tick ")).length, tickCount);
  assert.equal(lines.filter((line) => line.startsWith("fault ")).length, 0);
  assert.equal(
    lines.filter((line) => new RegExp(`^action ${PLAY_TONE_HEX} .+ async$`).test(line)).length,
    dispatchCount
  );
}

/** Builds and runs a fixture, checking it is fault-free and reproducible. */
function runToneFixture(
  name: string,
  rules: readonly ToneRuleSpec[],
  tickCount: number,
  structure: BrainStructure = "siblings"
): string {
  const image = buildImage(name, rules, structure);
  const first = runTrace(image, tickCount);
  assert.equal(runTrace(image, tickCount), first, "two fresh runs must render byte-identical traces");
  assertToneTrace(first, tickCount, rules.length);
  return first;
}

/**
 * Pins the `.mcprogram` / `.mcprogram.bin` / `.ticks.trace` golden triple of a
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
  structure: BrainStructure = "siblings"
): string {
  const jsonPath = fileURLToPath(new URL(`./__fixtures__/${name}.mcprogram`, import.meta.url));
  const binPath = fileURLToPath(new URL(`./__fixtures__/${name}.mcprogram.bin`, import.meta.url));
  const tracePath = fileURLToPath(new URL(`./__fixtures__/${name}.ticks.trace`, import.meta.url));

  if (!existsSync(jsonPath)) {
    const image = buildImage(name, rules, structure);
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

test("a bare beep sounds the default triangle 880 Hz tone for half a second", () => {
  const tickCount = resumeTickFor(DEFAULT_DURATION_MS);
  const trace = runToneGoldenFixture("play-tone-defaults", [{}], tickCount);
  assert.equal(countToneLines(trace), 1);
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, DEFAULT_FREQUENCY_HZ, DEFAULT_DURATION_MS, 1)), 1);
  assert.equal(
    tickOfLine(trace, (l) => l.startsWith("port speaker tone ")),
    1
  );
  // The rule parks for the tone's duration; its child marks the resume tick.
  assert.equal(
    tickOfLine(trace, (l) => l.startsWith("port display set-pixel ")),
    tickCount
  );
});

test("an explicit frequency sets the tone pitch and leaves the other defaults", () => {
  const trace = runToneFixture("play-tone-frequency", [{ frequencyHz: 440 }], resumeTickFor(DEFAULT_DURATION_MS));
  assert.equal(countToneLines(trace), 1);
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, 440, DEFAULT_DURATION_MS, 1)), 1);
});

test("an explicit duration sets the tone length in whole milliseconds and the lease it holds", () => {
  const tickCount = resumeTickFor(200);
  const trace = runToneFixture("play-tone-duration", [{ durationSeconds: 0.2 }], tickCount);
  assert.equal(countToneLines(trace), 1);
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, DEFAULT_FREQUENCY_HZ, 200, 1)), 1);
  assert.equal(
    tickOfLine(trace, (l) => l.startsWith("port display set-pixel ")),
    tickCount
  );
});

test("an explicit volume crosses the port as the tone's 0-1 fraction", () => {
  const trace = runToneFixture("play-tone-volume", [{ volume: 0.25 }], resumeTickFor(DEFAULT_DURATION_MS));
  assert.equal(countToneLines(trace), 1);
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, DEFAULT_FREQUENCY_HZ, DEFAULT_DURATION_MS, 0.25)), 1);
});

test("each wave-shape modifier selects its waveform word at the port", () => {
  // One awaited beep per wave shape, each nested under the one before it, so a
  // beep dispatches on the round the beep before it released the lease.
  const waveforms: readonly SpeakerToneWaveform[] = ["square", "sawtooth", "sine", "triangle"];
  const durationMs = 100;
  const rules = waveforms.map((waveform) => ({ waveform, durationSeconds: durationMs / 1000 }));
  const step = resumeTickFor(durationMs) - 1;
  const markerTick = 1 + waveforms.length * step;
  const trace = runToneGoldenFixture("play-tone-waveforms", rules, markerTick, "chain");

  assert.equal(countToneLines(trace), waveforms.length);
  for (let i = 0; i < waveforms.length; i++) {
    const line = toneLine(waveforms[i]!, DEFAULT_FREQUENCY_HZ, durationMs, 1);
    assert.equal(countLines(trace, line), 1);
    // Each tone lands on the tick the previous tone's lease predicts.
    assert.equal(
      tickOfLine(trace, (l) => l === line),
      1 + i * step,
      `dispatch tick of the ${waveforms[i]} beep`
    );
  }
  // The chain's last child lights a pixel once the final beep resolves.
  assert.equal(
    tickOfLine(trace, (l) => l.startsWith("port display set-pixel ")),
    markerTick
  );
});

test("a beep carrying a frequency, a duration, and a volume sets all three at the port", () => {
  const tickCount = resumeTickFor(200);
  const trace = runToneGoldenFixture(
    "play-tone-arguments",
    [{ frequencyHz: 262, durationSeconds: 0.2, volume: 0.25 }],
    tickCount
  );
  assert.equal(countToneLines(trace), 1);
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, 262, 200, 0.25)), 1);
  assert.equal(
    tickOfLine(trace, (l) => l.startsWith("port display set-pixel ")),
    tickCount
  );
});

test("a beep, a rest, and a beep sequence one after another, the rest holding the lease", () => {
  // Three awaited beeps nested one under the next; the middle one is a 0 Hz
  // rest, which sounds nothing but leases the speaker for its full duration.
  const durationMs = 200;
  const seconds = durationMs / 1000;
  const step = resumeTickFor(durationMs) - 1;
  const trace = runToneGoldenFixture(
    "play-tone-rest-sequence",
    [
      { frequencyHz: 440, durationSeconds: seconds },
      { frequencyHz: 0, durationSeconds: seconds },
      { frequencyHz: 660, durationSeconds: seconds },
    ],
    1 + 3 * step,
    "chain"
  );
  assert.equal(countToneLines(trace), 3);
  // The rest crosses the port at volume 0; the beeps around it at full volume.
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, 440, durationMs, 1)), 1);
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, 0, durationMs, 0)), 1);
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, 660, durationMs, 1)), 1);
  // The rest holds the lease for its duration: the beep after it dispatches a
  // full step later, exactly as the beep before it did.
  assert.equal(
    tickOfLine(trace, (l) => l === toneLine(DEFAULT_WAVEFORM, 0, durationMs, 0)),
    1 + step
  );
  assert.equal(
    tickOfLine(trace, (l) => l === toneLine(DEFAULT_WAVEFORM, 660, durationMs, 1)),
    1 + 2 * step
  );
  assert.equal(
    tickOfLine(trace, (l) => l.startsWith("port display set-pixel ")),
    1 + 3 * step
  );
});

test("a 0 Hz beep crosses the port as a silent rest and still holds the lease", () => {
  const tickCount = resumeTickFor(DEFAULT_DURATION_MS);
  const trace = runToneFixture("play-tone-rest", [{ frequencyHz: 0, volume: 0.5 }], tickCount);
  // The rest encodes at volume 0 whatever volume was asked for.
  assert.equal(countToneLines(trace), 1);
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, 0, DEFAULT_DURATION_MS, 0)), 1);
  // It leases the speaker for the full duration, so the rule still parks.
  assert.equal(
    tickOfLine(trace, (l) => l.startsWith("port display set-pixel ")),
    tickCount
  );
});

test("a negative frequency clamps to the silent rest at the low end of the pitch range", () => {
  const trace = runToneFixture("play-tone-clamp-low", [{ frequencyHz: -5 }], resumeTickFor(DEFAULT_DURATION_MS));
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, 0, DEFAULT_DURATION_MS, 0)), 1);
});

test("a frequency above the pitch range clamps to its top", () => {
  const trace = runToneFixture("play-tone-clamp-high", [{ frequencyHz: 20000 }], resumeTickFor(DEFAULT_DURATION_MS));
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, MAX_TONE_FREQUENCY_HZ, DEFAULT_DURATION_MS, 1)), 1);
});

test("a volume outside 0-1 clamps into the fraction range", () => {
  const loud = runToneFixture("play-tone-clamp-loud", [{ volume: 2 }], resumeTickFor(DEFAULT_DURATION_MS));
  assert.equal(countLines(loud, toneLine(DEFAULT_WAVEFORM, DEFAULT_FREQUENCY_HZ, DEFAULT_DURATION_MS, 1)), 1);

  const quiet = runToneFixture("play-tone-clamp-quiet", [{ volume: -1 }], resumeTickFor(DEFAULT_DURATION_MS));
  assert.equal(countLines(quiet, toneLine(DEFAULT_WAVEFORM, DEFAULT_FREQUENCY_HZ, DEFAULT_DURATION_MS, 0)), 1);
});

test("a negative duration sounds nothing, crosses no port, and resolves at dispatch", () => {
  const trace = runToneFixture("play-tone-negative-duration", [{ durationSeconds: -1 }], 3);
  assert.equal(countToneLines(trace), 0);
  // The rule continues on the dispatch tick, as a busy-speaker drop does.
  assert.equal(
    tickOfLine(trace, (l) => l.startsWith("port display set-pixel ")),
    1
  );
});

test("a zero duration crosses the port and resolves on the first lease settle", () => {
  const trace = runToneFixture("play-tone-zero-duration", [{ durationSeconds: 0 }], 3);
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, DEFAULT_FREQUENCY_HZ, 0, 1)), 1);
  assert.equal(
    tickOfLine(trace, (l) => l.startsWith("port display set-pixel ")),
    2
  );
});

test("a beep dispatched while the speaker is busy is silently dropped", () => {
  const tickCount = resumeTickFor(DEFAULT_DURATION_MS);
  const trace = runToneGoldenFixture("play-tone-dropped", [{}, { frequencyHz: 440 }], tickCount);
  // Only the holder's tone crosses the port; the competitor's is dropped.
  assert.equal(countToneLines(trace), 1);
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, DEFAULT_FREQUENCY_HZ, DEFAULT_DURATION_MS, 1)), 1);
  // The dropped rule resolves at once (its child marks tick 1); the holder
  // resumes after its duration.
  assert.equal(
    tickOfLine(trace, (l) => l.startsWith("port display set-pixel ")),
    1
  );
  assert.equal(
    lastTickOfLine(trace, (l) => l.startsWith("port display set-pixel ")),
    tickCount
  );
});

test("a beep with immediately preempts the holder, whose rule resumes", () => {
  const tickCount = resumeTickFor(DEFAULT_DURATION_MS);
  const holderNaturalResume = resumeTickFor(1000);
  const trace = runToneGoldenFixture(
    "play-tone-preempt",
    [{ frequencyHz: 440, durationSeconds: 1 }, { immediately: true }],
    tickCount
  );
  // Both tones cross the port on the dispatch tick: the holder's, then the
  // preemptor's after the preempt released the lease.
  assert.equal(countToneLines(trace), 2);
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, 440, 1000, 1)), 1);
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, DEFAULT_FREQUENCY_HZ, DEFAULT_DURATION_MS, 1)), 1);
  assert.equal(
    lastTickOfLine(trace, (l) => l.startsWith("port speaker tone ")),
    1
  );
  // The preempted holder resumes well before its own tone would have ended.
  const firstPixelTick = tickOfLine(trace, (l) => l.startsWith("port display set-pixel "));
  assert.ok(firstPixelTick > 0 && firstPixelTick < holderNaturalResume);
  assert.equal(
    lastTickOfLine(trace, (l) => l.startsWith("port display set-pixel ")),
    tickCount
  );
});

test("a beep in background keeps its lease while the issuing rule continues", () => {
  // The lease settles on tick time after the duration; stop short of that.
  const tickCount = resumeTickFor(DEFAULT_DURATION_MS) - 3;
  const trace = runToneGoldenFixture("play-tone-background", [{ inBackground: true }], tickCount);
  assert.equal(countLines(trace, toneLine(DEFAULT_WAVEFORM, DEFAULT_FREQUENCY_HZ, DEFAULT_DURATION_MS, 1)), 1);
  // The handle resolves at dispatch: the child drains on the dispatch tick while
  // the tone still holds the speaker lease for several more thinks.
  assert.equal(trace.split("\n").filter((line) => line.startsWith("port display set-pixel ")).length, 1);
  assert.equal(
    tickOfLine(trace, (l) => l.startsWith("port display set-pixel ")),
    1
  );
});

test("a non-finite frequency, duration, or volume reads as its default", () => {
  const nonFinite = [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY];
  for (const value of nonFinite) {
    assert.deepEqual(dispatchedTone({ frequencyHz: value }), {
      waveform: DEFAULT_WAVEFORM,
      frequencyHz: DEFAULT_FREQUENCY_HZ,
      durationMs: DEFAULT_DURATION_MS,
      volume: 1,
    });
    assert.deepEqual(dispatchedTone({ durationSeconds: value }), {
      waveform: DEFAULT_WAVEFORM,
      frequencyHz: DEFAULT_FREQUENCY_HZ,
      durationMs: DEFAULT_DURATION_MS,
      volume: 1,
    });
    assert.deepEqual(dispatchedTone({ volume: value }), {
      waveform: DEFAULT_WAVEFORM,
      frequencyHz: DEFAULT_FREQUENCY_HZ,
      durationMs: DEFAULT_DURATION_MS,
      volume: 1,
    });
  }
});

test("the speaker snapshot carries the playing tone so the simulator can render it", () => {
  const speaker = new MicroBitSpeaker();
  const command = mkSpeakerToneCommand("sine", 440, 250, 0.5);
  speaker.playTone(command, 0, () => {});
  assert.deepEqual(speaker.snapshot().playing, {
    name: "",
    tone: command,
    startedAt: 0,
    durationMs: 250,
    playId: 1,
  });
});

/** The tone slot values a direct dispatch fills, keyed by the argument they set. */
interface ToneArgs {
  readonly frequencyHz?: number;
  readonly durationSeconds?: number;
  readonly volume?: number;
}

/**
 * Dispatches the play-tone actuator directly with `args` in its named slots and
 * returns the command it hands the speaker port. Accepts non-finite values.
 */
function dispatchedTone(args: ToneArgs): SpeakerToneCommand {
  const env = createMicroBitV2Environment();
  const microbit = new MicroBit();
  const entry = env.brainServices.runtime.functions.get(MicroBitV2HostActions.PlayTone.key);
  assert.ok(entry, "the play-tone actuator should be registered");
  assert.equal(entry.isAsync, true);

  const slots = List.empty<Value>();
  for (let i = 0; i < entry.callDef.argSlots.size(); i++) {
    slots.push(NIL_VALUE);
  }
  if (args.frequencyHz !== undefined) {
    slots.set(
      getSlotId(entry.callDef, mkParameterTileId(CoreParameterId.AnonymousNumber)),
      mkNumberValue(args.frequencyHz)
    );
  }
  if (args.durationSeconds !== undefined) {
    slots.set(
      getSlotId(entry.callDef, mkParameterTileId(WodalMicroBitV2ParameterId.Duration)),
      mkNumberValue(args.durationSeconds)
    );
  }
  if (args.volume !== undefined) {
    slots.set(
      getSlotId(entry.callDef, mkParameterTileId(WodalMicroBitV2ParameterId.Volume)),
      mkNumberValue(args.volume)
    );
  }

  let dispatched: SpeakerToneCommand | undefined;
  const devicePlayTone = microbit.speaker.playTone.bind(microbit.speaker);
  microbit.speaker.playTone = (command, requestTime, onEnd) => {
    dispatched = command;
    devicePlayTone(command, requestTime, onEnd);
  };

  const handle: AsyncHandle = { id: 1, resolve: () => {}, reject: () => {}, cancel: () => {} };
  entry.fn.exec(createExecutionContext(env, microbit), slots, handle);
  assert.ok(dispatched, "the actuator should hand a command to the speaker port");
  return dispatched;
}

/** A minimal execution context binding the device a directly dispatched actuator drives. */
function createExecutionContext(env: WendooEnvironment, microbit: MicroBit): ExecutionContext {
  return {
    services: {
      ...(env.brainServices as unknown as Record<string, unknown>),
      brain: { ruleVars: { getByName: () => NIL_VALUE, setByName: () => {} } },
    } as unknown as ExecutionContext["services"],
    getVariableBySlot: () => NIL_VALUE,
    setVariableBySlot: () => {},
    getSystemVarBySlot: () => NIL_VALUE,
    setSystemVarBySlot: () => {},
    data: { microbit },
    time: 0,
    dt: 0,
    currentTick: 0,
  };
}
