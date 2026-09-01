/**
 * Golden observable traces for the `otherwise` and `then` rule trigger modes:
 * REAL compiled brains whose rules carry a trigger mode on the document model,
 * built through the tile API and linked by the WODAL build kernel. Each rule
 * marks itself by lighting its own display pixel, so a trace block reads as the
 * set of rules that fired that think.
 *
 * Two families live here. The `otherwise-chain-*` fixtures cover the mode's
 * ladder: a three-rule ladder driven through head-fires, middle-fires, and
 * none-fire thinks, a ladder whose head parks on an awaited actuator, a ladder
 * headed by a `then` rule exercised during its wait and after its fire, and a
 * bare else pair. The `then-*` fixtures cover the await model:
 * a bare `then` after a childless sibling, after a sibling with an empty DO,
 * after a sibling that never fires, and after a sibling whose child parks
 * across thinks; a filtered `then` whose expression holds and does not hold at
 * the wake think; root-level and child-level chains of three; a chain whose
 * middle subject skips; and a page exit during a wait followed by re-entry.
 *
 * Two lines carry the mode machinery across the observable surface. An
 * `otherwise` rule's arming read is the `otherwise` host sensor, so it renders
 * as an `action 8` line carrying its boolean answer. A `then` rule's trigger is
 * the asynchronous rule-trigger host action, so each evaluation renders as an
 * `action 9` line; a `then` waiting across thinks dispatches nothing, which
 * separates waiting from skipping when neither produces a pixel.
 *
 * The JSON, binary, and rendered trace are pinned beside this spec as the
 * cross-VM conformance fixtures: the C++ VM parity test
 * (cpp/test/trace-parity.test.cpp) loads each binary, replays the same
 * schedule, and byte-compares the trace.
 */

import assert from "node:assert/strict";
import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { test } from "node:test";
import { fileURLToPath } from "node:url";
import {
  BrainTileLiteralDef,
  CoreHostActions,
  CoreTypeIds,
  type HostActionIds,
  mkActuatorTileId,
  mkModifierTileId,
  mkParameterTileId,
  mkSensorTileId,
  type WendooEnvironment,
} from "@wendoo/core/app";
import { RuleTriggerMode } from "@wendoo/core/brain";
import { BrainDef, type BrainPageDef, type BrainRuleDef } from "@wendoo/core/brain/model";
import { type LinkedBrainProgram, linkedBrainProgramToJson, Op } from "@wendoo/core/runtime";
import { type IncomingRadioPacket, RadioPacketType, radioNumberIsInteger } from "../../../core/radio";
import { buildWodalProgramImage } from "../../../wendoo/build-kernel";
import { getWodalDeviceProfile, WodalDeviceProfileId } from "../../../wendoo/device-profile";
import { shouldWriteGolden } from "../../../wendoo/golden-regeneration";
import { serializeWodalProgramImageJson, type WodalProgramImage } from "../../../wendoo/program-image";
import { parseWodalProgramImageBytes, wodalProgramBytes } from "../../../wendoo/program-image-binary";
import { MicroBit } from "../microbit";
import { createMicroBitV2Environment } from "./environment";
import { ObservableTraceWriter, observableTraceVmEvents } from "./observable-trace";
import { WodalMicroBitRuntime } from "./runtime";
import { MicroBitV2HostActions, WodalMicroBitV2ModifierId, WodalMicroBitV2ParameterId } from "./tile-ids";

const BUTTON_A = MicroBitV2HostActions.ButtonA;
const BUTTON_B = MicroBitV2HostActions.ButtonB;
const RADIO_NUMBER = MicroBitV2HostActions.RadioReceiveNumber;
const HELD = WodalMicroBitV2ModifierId.Held;

/** Fixed metadata stamped on every injected packet; the tile sensor reads only the value. */
const INJECT_RSSI = -42;

/** Builds a number packet the way a real sender would. */
function numberPacket(value: number): IncomingRadioPacket {
  return {
    type: radioNumberIsInteger(value) ? RadioPacketType.Number : RadioPacketType.Double,
    group: 0,
    value,
    name: "",
    text: "",
    bytes: new Uint8Array(0),
    rssi: INJECT_RSSI,
    serial: 0,
    time: 0,
  };
}

/** Trace-line prefix of one rule-trigger host-action dispatch. */
const TRIGGER_LINE_PREFIX = `action ${CoreHostActions.RuleTrigger.actionId.toString(16)} `;

/** Think cadence of the pixel-only fixtures, in milliseconds. */
const FAST_TICK_MS = 16;

/**
 * Think cadence of the fixtures whose subject parks on the display scroll, in
 * milliseconds. The default scroll spans six thinks at this cadence.
 */
const SCROLL_TICK_MS = 1100;

/** The WHEN side of one rule in a fixture's tree. */
type WhenSpec =
  /** No WHEN condition: an armed rule fires every think it evaluates. */
  | { readonly kind: "empty" }
  /** A device sensor, optionally carrying a modifier tile. */
  | { readonly kind: "sensor"; readonly sensor: HostActionIds; readonly modifierId?: string };

/** One rule of a fixture's tree: its trigger mode, its WHEN, its DO, and its children. */
interface RuleSpec {
  /** Trigger mode the rule carries; `when` when omitted. */
  readonly trigger?: RuleTriggerMode;
  readonly when: WhenSpec;
  /** Display coordinate this rule lights when it fires; omitted for a rule with an empty DO. */
  readonly pixel?: readonly [number, number];
  /** Whether this rule's DO scrolls text, the awaited action that parks it. */
  readonly scrolls?: boolean;
  /** One-based page number this rule switches to when it fires. */
  readonly switchToPage?: number;
  readonly children?: readonly RuleSpec[];
}

/** One scheduled think: device input applied before the time advance, then the advance. */
interface ScheduleStep {
  readonly advanceMs: number;
  readonly a?: boolean;
  readonly b?: boolean;
  readonly inject?: readonly IncomingRadioPacket[];
}

/** One trigger-mode golden: its rule tree, its input schedule, and what each think must show. */
interface TriggerFixture {
  readonly name: string;
  /** Root rules per page, in page order. */
  readonly pages: readonly (readonly RuleSpec[])[];
  readonly schedule: readonly ScheduleStep[];
  /**
   * The marker pixels expected in each think, as `[x, y]` pairs in trace order.
   * One entry per scheduled think; an empty entry means no rule marked that think.
   */
  readonly expectedPixels: readonly (readonly (readonly [number, number])[])[];
  /**
   * Rule-trigger dispatches expected in each think, one entry per scheduled
   * think. A `then` rule waiting across thinks dispatches nothing, so this
   * separates a waiting rule from a skipping one when neither marks. Omitted
   * for a fixture with no `then` rule.
   */
  readonly expectedTriggerDispatches?: readonly number[];
  /**
   * Scroll dispatches expected in each think, one entry per scheduled think.
   * A rule that parks on its scroll marks nothing while it waits, so this is the
   * per-think firing record of a chain whose links all park. Omitted for a
   * fixture whose rules do not scroll.
   */
  readonly expectedScrolls?: readonly number[];
  /** Opcodes the committed program must carry. */
  readonly requiredOpcodes?: readonly number[];
}

const sensorWhen = (sensor: HostActionIds, modifierId?: string): WhenSpec => ({
  kind: "sensor",
  sensor,
  modifierId,
});

const buttonAHeld = sensorWhen(BUTTON_A, HELD);
const buttonBHeld = sensorWhen(BUTTON_B, HELD);

/** `then` links following the head of the long parked chain. */
const PARKED_CHAIN_LINKS = 12;

/**
 * Think cadence of the long parked chain, in milliseconds. One think is longer
 * than the default scroll, so each link's animation elapses inside the think
 * after the one that started it.
 */
const PARKED_CHAIN_TICK_MS = 5000;

/**
 * Thinks one link of the long parked chain occupies: the think it fires and
 * dispatches its scroll, the think that scroll's duration elapses, and the think
 * its awaited resume settles the link for the next link's trigger.
 */
const PARKED_CHAIN_THINKS_PER_LINK = 3;

/** Think index at which the chain's head scrolls, the subject press landing on think index 1. */
const PARKED_CHAIN_HEAD_SCROLL = 1;

/** Scheduled thinks of the long parked chain. */
const PARKED_CHAIN_THINKS = PARKED_CHAIN_HEAD_SCROLL + (PARKED_CHAIN_LINKS + 1) * PARKED_CHAIN_THINKS_PER_LINK + 3;

/** The head plus {@link PARKED_CHAIN_LINKS} `then` links, each parking on its own scroll. */
function parkedChainRules(): RuleSpec[] {
  const rules: RuleSpec[] = [{ when: buttonAHeld, scrolls: true }];
  for (let i = 0; i < PARKED_CHAIN_LINKS; i++) {
    rules.push({ trigger: RuleTriggerMode.Then, when: { kind: "empty" }, scrolls: true });
  }
  return rules;
}

/**
 * The chain's scrolls over {@link PARKED_CHAIN_THINKS} thinks: the head at
 * {@link PARKED_CHAIN_HEAD_SCROLL}, then one link every
 * {@link PARKED_CHAIN_THINKS_PER_LINK} thinks, in document order.
 */
function parkedChainExpectedScrolls(): number[] {
  const scrolls = Array.from({ length: PARKED_CHAIN_THINKS }, () => 0);
  for (let i = 0; i <= PARKED_CHAIN_LINKS; i++) {
    const think = PARKED_CHAIN_HEAD_SCROLL + i * PARKED_CHAIN_THINKS_PER_LINK;
    if (think < PARKED_CHAIN_THINKS) {
      scrolls[think] = 1;
    }
  }
  return scrolls;
}

const FIXTURES: readonly TriggerFixture[] = [
  {
    // The ladder: exactly one branch marks each think. On think 4 the head
    // fires while the middle's expression also holds, and both the middle and
    // the tail stay quiet.
    name: "otherwise-chain-ladder",
    pages: [
      [
        { when: buttonAHeld, pixel: [0, 0] },
        { trigger: RuleTriggerMode.Otherwise, when: buttonBHeld, pixel: [1, 0] },
        { trigger: RuleTriggerMode.Otherwise, when: { kind: "empty" }, pixel: [2, 0] },
      ],
    ],
    schedule: [
      { advanceMs: FAST_TICK_MS },
      { advanceMs: FAST_TICK_MS, a: true },
      { advanceMs: FAST_TICK_MS, a: false, b: true },
      { advanceMs: FAST_TICK_MS, a: true },
      { advanceMs: FAST_TICK_MS, a: false, b: false },
    ],
    expectedPixels: [[[2, 0]], [[0, 0]], [[1, 0]], [[0, 0]], [[2, 0]]],
    requiredOpcodes: [Op.WHEN_END_CHAIN],
  },
  {
    // A ladder whose middle branch is armed by a presence-gated sensor: it
    // fires on a delivered 0 -- present but falsy -- and stays quiet on the
    // thinks nothing is delivered, so the chain gate takes the presence form.
    name: "otherwise-chain-presence-gated",
    pages: [
      [
        { when: buttonAHeld, pixel: [0, 0] },
        { trigger: RuleTriggerMode.Otherwise, when: sensorWhen(RADIO_NUMBER), pixel: [1, 0] },
        { trigger: RuleTriggerMode.Otherwise, when: { kind: "empty" }, pixel: [2, 0] },
      ],
    ],
    schedule: [
      { advanceMs: FAST_TICK_MS },
      { advanceMs: FAST_TICK_MS, inject: [numberPacket(0)] },
      { advanceMs: FAST_TICK_MS },
      { advanceMs: FAST_TICK_MS, inject: [numberPacket(9)] },
      { advanceMs: FAST_TICK_MS, a: true },
    ],
    expectedPixels: [[[2, 0]], [[1, 0]], [[2, 0]], [[1, 0]], [[0, 0]]],
    requiredOpcodes: [Op.WHEN_END_PRESENT_CHAIN, Op.WHEN_END_CHAIN],
  },
  {
    // A head parked on its awaited scroll keeps the whole ladder quiet: its
    // record holds the fire it recorded, so neither branch below it arms. The
    // tail marks think 1, where the button-B call site seeds its baseline and
    // reads false; the middle branch takes over once the head settles.
    name: "otherwise-chain-parked-head",
    pages: [
      [
        { when: buttonAHeld, scrolls: true },
        { trigger: RuleTriggerMode.Otherwise, when: buttonBHeld, pixel: [1, 0] },
        { trigger: RuleTriggerMode.Otherwise, when: { kind: "empty" }, pixel: [2, 0] },
      ],
    ],
    schedule: [
      { advanceMs: SCROLL_TICK_MS, b: true },
      { advanceMs: SCROLL_TICK_MS, a: true },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS, a: false },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
    ],
    expectedPixels: [[[2, 0]], [], [], [], [], [], [], [[1, 0]], [[1, 0]], [[1, 0]]],
  },
  {
    // A ladder headed by a `then` rule: the else branch runs on every think the
    // `then` has not fired, the whole wait included, and goes quiet the think
    // the `then` fires.
    name: "otherwise-chain-then-head",
    pages: [
      [
        { when: buttonAHeld, scrolls: true },
        { trigger: RuleTriggerMode.Then, when: { kind: "empty" }, pixel: [1, 0] },
        { trigger: RuleTriggerMode.Otherwise, when: { kind: "empty" }, pixel: [2, 0] },
      ],
    ],
    schedule: [
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS, a: true },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS, a: false },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
    ],
    expectedPixels: [
      [[2, 0]],
      [[2, 0]],
      [[2, 0]],
      [[2, 0]],
      [[2, 0]],
      [[2, 0]],
      [[2, 0]],
      [[1, 0]],
      [[2, 0]],
      [[2, 0]],
      [[2, 0]],
      [[2, 0]],
    ],
    expectedTriggerDispatches: [1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1],
  },
  {
    // A bare else on the second rule: the pair runs as an else branch and the
    // compiled program carries the chain gate.
    name: "otherwise-chain-migrated-pair",
    pages: [
      [
        { when: buttonAHeld, pixel: [0, 0] },
        { trigger: RuleTriggerMode.Otherwise, when: { kind: "empty" }, pixel: [1, 0] },
      ],
    ],
    schedule: [
      { advanceMs: FAST_TICK_MS },
      { advanceMs: FAST_TICK_MS, a: true },
      { advanceMs: FAST_TICK_MS },
      { advanceMs: FAST_TICK_MS, a: false },
      { advanceMs: FAST_TICK_MS },
    ],
    expectedPixels: [[[1, 0]], [[0, 0]], [[0, 0]], [[1, 0]], [[1, 0]]],
    requiredOpcodes: [Op.WHEN_END_CHAIN],
  },
  {
    // A childless sibling settles inside its own turn of the round, so the
    // `then` continues the same think.
    name: "then-after-childless-sibling",
    pages: [
      [
        { when: buttonAHeld, pixel: [0, 0] },
        { trigger: RuleTriggerMode.Then, when: { kind: "empty" }, pixel: [1, 0] },
      ],
    ],
    schedule: [
      { advanceMs: FAST_TICK_MS },
      { advanceMs: FAST_TICK_MS, a: true },
      { advanceMs: FAST_TICK_MS },
      { advanceMs: FAST_TICK_MS, a: false },
      { advanceMs: FAST_TICK_MS },
    ],
    expectedPixels: [
      [],
      [
        [0, 0],
        [1, 0],
      ],
      [
        [0, 0],
        [1, 0],
      ],
      [],
      [],
    ],
    expectedTriggerDispatches: [1, 1, 1, 1, 1],
  },
  {
    // A subject whose WHEN never holds: the `then` reaches its trigger every
    // think and skips every think, the first one included, where no rule has
    // yet written a firing record.
    name: "then-silent-sibling",
    pages: [
      [
        { when: buttonAHeld, pixel: [0, 0] },
        { trigger: RuleTriggerMode.Then, when: { kind: "empty" }, pixel: [1, 0] },
      ],
    ],
    schedule: [{ advanceMs: FAST_TICK_MS }, { advanceMs: FAST_TICK_MS }, { advanceMs: FAST_TICK_MS }],
    expectedPixels: [[], [], []],
    expectedTriggerDispatches: [1, 1, 1],
  },
  {
    // A fired sibling with an empty DO still completes its cluster, so the
    // `then` runs even though its subject marks nothing.
    name: "then-after-empty-do-sibling",
    pages: [[{ when: buttonAHeld }, { trigger: RuleTriggerMode.Then, when: { kind: "empty" }, pixel: [1, 0] }]],
    schedule: [
      { advanceMs: FAST_TICK_MS },
      { advanceMs: FAST_TICK_MS, a: true },
      { advanceMs: FAST_TICK_MS },
      { advanceMs: FAST_TICK_MS, a: false },
    ],
    expectedPixels: [[], [[1, 0]], [[1, 0]], []],
    expectedTriggerDispatches: [1, 1, 1, 1],
  },
  {
    // Cluster semantics: the subject's own DO finishes in its firing think, its
    // child parks on the awaited scroll, and the `then` waits for the whole
    // subtree, waking one round after the last descendant finishes.
    name: "then-after-parked-child",
    pages: [
      [
        {
          when: buttonAHeld,
          pixel: [0, 0],
          children: [{ when: { kind: "empty" }, scrolls: true }],
        },
        { trigger: RuleTriggerMode.Then, when: { kind: "empty" }, pixel: [1, 0] },
      ],
    ],
    schedule: [
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS, a: true },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS, a: false },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
    ],
    expectedPixels: [[], [[0, 0]], [], [], [], [], [], [[1, 0]], [], [], [], []],
    expectedTriggerDispatches: [1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1],
  },
  {
    // The expression is evaluated at the wake think and filters the completion:
    // the completion is taken when the sensor holds and skipped when it does
    // not. Think 2 is the first completion, where the filter's call site seeds
    // its baseline and reads false; thinks 3 to 5 alternate the sensor.
    name: "then-filtered",
    pages: [
      [
        { when: buttonAHeld, pixel: [0, 0] },
        { trigger: RuleTriggerMode.Then, when: buttonBHeld, pixel: [1, 0] },
      ],
    ],
    schedule: [
      { advanceMs: FAST_TICK_MS },
      { advanceMs: FAST_TICK_MS, a: true },
      { advanceMs: FAST_TICK_MS, b: true },
      { advanceMs: FAST_TICK_MS, b: false },
      { advanceMs: FAST_TICK_MS, b: true },
      { advanceMs: FAST_TICK_MS, a: false },
    ],
    expectedPixels: [
      [],
      [[0, 0]],
      [
        [0, 0],
        [1, 0],
      ],
      [[0, 0]],
      [
        [0, 0],
        [1, 0],
      ],
      [],
    ],
    expectedTriggerDispatches: [1, 1, 1, 1, 1, 1],
  },
  {
    // A root-level chain of three, stepped by a parked head: each link wakes one
    // round after the link above it settles.
    name: "then-chain-root",
    pages: [
      [
        { when: buttonAHeld, scrolls: true },
        { trigger: RuleTriggerMode.Then, when: { kind: "empty" }, pixel: [1, 0] },
        { trigger: RuleTriggerMode.Then, when: { kind: "empty" }, pixel: [2, 0] },
      ],
    ],
    schedule: [
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS, a: true },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS, a: false },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
    ],
    expectedPixels: [[], [], [], [], [], [], [], [[1, 0]], [[2, 0]], [], [], []],
    expectedTriggerDispatches: [2, 2, 0, 0, 0, 0, 0, 0, 1, 2, 2, 2],
  },
  {
    // The same chain at child level, under a synchronous parent: every link
    // settles inside its own turn, so the whole sequence runs in one think.
    name: "then-chain-child",
    pages: [
      [
        {
          when: buttonAHeld,
          children: [
            { when: { kind: "empty" }, pixel: [0, 1] },
            { trigger: RuleTriggerMode.Then, when: { kind: "empty" }, pixel: [1, 1] },
            { trigger: RuleTriggerMode.Then, when: { kind: "empty" }, pixel: [2, 1] },
          ],
        },
      ],
    ],
    schedule: [
      { advanceMs: FAST_TICK_MS },
      { advanceMs: FAST_TICK_MS, a: true },
      { advanceMs: FAST_TICK_MS },
      { advanceMs: FAST_TICK_MS, a: false },
    ],
    expectedPixels: [
      [],
      [
        [0, 1],
        [1, 1],
        [2, 1],
      ],
      [
        [0, 1],
        [1, 1],
        [2, 1],
      ],
      [],
    ],
    expectedTriggerDispatches: [0, 2, 2, 0],
  },
  {
    // A filter that does not hold takes the whole spine down: the third link
    // receives the skip as its trigger answer and never evaluates its own
    // expression. Think 2 is the first completion, where the middle link's call
    // site seeds its baseline and reads false.
    name: "then-chain-skipped-middle",
    pages: [
      [
        { when: buttonAHeld, pixel: [0, 0] },
        { trigger: RuleTriggerMode.Then, when: buttonBHeld, pixel: [1, 0] },
        { trigger: RuleTriggerMode.Then, when: { kind: "empty" }, pixel: [2, 0] },
      ],
    ],
    schedule: [
      { advanceMs: FAST_TICK_MS },
      { advanceMs: FAST_TICK_MS, a: true },
      { advanceMs: FAST_TICK_MS, b: true },
      { advanceMs: FAST_TICK_MS, b: false },
      { advanceMs: FAST_TICK_MS, b: true },
      { advanceMs: FAST_TICK_MS, a: false },
    ],
    expectedPixels: [
      [],
      [[0, 0]],
      [
        [0, 0],
        [1, 0],
        [2, 0],
      ],
      [[0, 0]],
      [
        [0, 0],
        [1, 0],
        [2, 0],
      ],
      [],
    ],
    expectedTriggerDispatches: [2, 2, 2, 2, 2, 2],
  },
  {
    // Leaving the page while the `then` waits cancels it, and re-entry starts
    // clean: the cancelled wait never marks, and the subject's next firing runs
    // a fresh wait through to its own completion. The subject's next press
    // lands on think 10, after the display has finished the scroll the
    // cancelled firing started; a scroll issued to a busy display resolves at
    // dispatch and parks nothing.
    name: "then-page-reentry",
    pages: [
      [
        { when: buttonAHeld, scrolls: true },
        { trigger: RuleTriggerMode.Then, when: { kind: "empty" }, pixel: [1, 0] },
        { when: buttonBHeld, switchToPage: 2 },
      ],
      [
        { when: { kind: "empty" }, pixel: [4, 4] },
        { when: { kind: "empty" }, switchToPage: 1 },
      ],
    ],
    schedule: [
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS, a: true },
      { advanceMs: SCROLL_TICK_MS, a: false, b: true },
      { advanceMs: SCROLL_TICK_MS, b: false },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS, a: true },
      { advanceMs: SCROLL_TICK_MS, a: false },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
      { advanceMs: SCROLL_TICK_MS },
    ],
    expectedPixels: [[], [], [], [[4, 4]], [], [], [], [], [], [], [], [], [], [], [], [[1, 0]], [], []],
    expectedTriggerDispatches: [1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1],
  },
  {
    // A chain longer than the microbit-v2 profile's concurrent-handle cap, every
    // link parking on its own awaited scroll. Each waiting link holds a pending
    // trigger handle for the whole wait, and those handles are uncapped, so the
    // chain marches to its last link: the cap bounds concurrent device work, not
    // chain length.
    name: "then-chain-parked-links",
    pages: [parkedChainRules()],
    schedule: Array.from({ length: PARKED_CHAIN_THINKS }, (_, i) => ({
      advanceMs: PARKED_CHAIN_TICK_MS,
      ...(i === 1 ? { a: true } : {}),
      ...(i === 2 ? { a: false } : {}),
    })),
    expectedPixels: Array.from({ length: PARKED_CHAIN_THINKS }, () => []),
    expectedScrolls: parkedChainExpectedScrolls(),
  },
];

function fixturePath(name: string, extension: string): string {
  return fileURLToPath(new URL(`./__fixtures__/${name}.${extension}`, import.meta.url));
}

/** Appends `set pixel` at `(x, y)` to `rule`'s DO, freezing the coordinates as literal tiles. */
function appendSetPixel(env: WendooEnvironment, brainDef: BrainDef, rule: BrainRuleDef, x: number, y: number): void {
  const tiles = env.brainServices.edit.tiles;
  const setPixel = tiles.get(mkActuatorTileId(MicroBitV2HostActions.DisplaySetPixel.key));
  const xParam = tiles.get(mkParameterTileId(WodalMicroBitV2ParameterId.X));
  const yParam = tiles.get(mkParameterTileId(WodalMicroBitV2ParameterId.Y));
  assert.ok(setPixel);
  assert.ok(xParam);
  assert.ok(yParam);
  rule.do().appendTile(setPixel);
  rule.do().appendTile(xParam);
  const xLiteral = new BrainTileLiteralDef(CoreTypeIds.Number, x, {}, env.brainServices);
  brainDef.catalog().registerTileDef(xLiteral);
  rule.do().appendTile(xLiteral);
  rule.do().appendTile(yParam);
  const yLiteral = new BrainTileLiteralDef(CoreTypeIds.Number, y, {}, env.brainServices);
  brainDef.catalog().registerTileDef(yLiteral);
  rule.do().appendTile(yLiteral);
}

/** Appends the display scroll actuator to `rule`'s DO, the awaited action that parks it. */
function appendScroll(env: WendooEnvironment, rule: BrainRuleDef): void {
  const scroll = env.brainServices.edit.tiles.get(mkActuatorTileId(MicroBitV2HostActions.DisplayScroll.key));
  assert.ok(scroll);
  rule.do().appendTile(scroll);
}

/** Appends `switch page <pageNumber>` to `rule`'s DO. */
function appendSwitchPage(env: WendooEnvironment, brainDef: BrainDef, rule: BrainRuleDef, pageNumber: number): void {
  const switchPage = env.brainServices.edit.tiles.get(mkActuatorTileId(CoreHostActions.SwitchPage.key));
  assert.ok(switchPage);
  rule.do().appendTile(switchPage);
  const pageLiteral = new BrainTileLiteralDef(CoreTypeIds.Number, pageNumber, {}, env.brainServices);
  brainDef.catalog().registerTileDef(pageLiteral);
  rule.do().appendTile(pageLiteral);
}

/** Appends `spec`'s WHEN tiles to `rule`. */
function appendWhen(env: WendooEnvironment, rule: BrainRuleDef, spec: WhenSpec): void {
  const tiles = env.brainServices.edit.tiles;
  switch (spec.kind) {
    case "empty":
      return;
    case "sensor": {
      const sensorTile = tiles.get(mkSensorTileId(spec.sensor.key));
      assert.ok(sensorTile);
      rule.when().appendTile(sensorTile);
      if (spec.modifierId !== undefined) {
        const modifierTile = tiles.get(mkModifierTileId(spec.modifierId));
        assert.ok(modifierTile);
        rule.when().appendTile(modifierTile);
      }
      return;
    }
  }
}

/** Fills `rule` from `spec` and recursively appends its children. */
function fillRule(env: WendooEnvironment, brainDef: BrainDef, rule: BrainRuleDef, spec: RuleSpec): void {
  if (spec.trigger !== undefined) {
    rule.setTrigger(spec.trigger);
  }
  appendWhen(env, rule, spec.when);
  if (spec.pixel !== undefined) {
    appendSetPixel(env, brainDef, rule, spec.pixel[0], spec.pixel[1]);
  }
  if (spec.scrolls === true) {
    appendScroll(env, rule);
  }
  if (spec.switchToPage !== undefined) {
    appendSwitchPage(env, brainDef, rule, spec.switchToPage);
  }
  for (const child of spec.children ?? []) {
    fillRule(env, brainDef, rule.appendNewRule() as BrainRuleDef, child);
  }
}

/** Builds a fixture's authored brain: one page per entry, one root rule per `RuleSpec`. */
function authorBrainDef(env: WendooEnvironment, fixture: TriggerFixture): BrainDef {
  const brainDef = BrainDef.emptyBrainDef(env.brainServices, `${fixture.name} brain`);
  for (let p = 0; p < fixture.pages.length; p++) {
    if (p > 0) {
      assert.ok(brainDef.appendNewPage().success);
    }
    const page = brainDef.pages().get(p)! as BrainPageDef;
    const specs = fixture.pages[p]!;
    for (let r = 0; r < specs.length; r++) {
      // Every page starts with one empty rule; later roots are appended.
      const rule = (r === 0 ? page.children().get(0)! : page.appendNewRule()) as BrainRuleDef;
      fillRule(env, brainDef, rule, specs[r]!);
    }
  }
  return brainDef;
}

function buildImage(env: WendooEnvironment, brainDef: BrainDef): WodalProgramImage<LinkedBrainProgram> {
  const built = buildWodalProgramImage({
    brainDef,
    environment: env,
    deviceProfile: getWodalDeviceProfile(WodalDeviceProfileId.MICROBIT_V2),
  });
  if (!built.ok) {
    assert.fail(`expected a successful build: ${JSON.stringify(built.errors)}`);
  }
  return built.image;
}

/** Writes the JSON `.mcprogram` golden if missing, freezing the brain's generated ids. */
function ensureJsonGolden(jsonPath: string, fixture: TriggerFixture): void {
  if (existsSync(jsonPath)) {
    return;
  }
  const env = createMicroBitV2Environment();
  const image = buildImage(env, authorBrainDef(env, fixture));
  writeFileSync(
    jsonPath,
    serializeWodalProgramImageJson({ ...image, program: linkedBrainProgramToJson(image.program) })
  );
}

/** The set of opcodes the committed program JSON at `jsonPath` carries. */
function committedOpcodes(jsonPath: string): Set<number> {
  const golden = JSON.parse(readFileSync(jsonPath, "utf8")) as {
    program: { program: { functions: { code: { op: number }[] }[] } };
  };
  const opcodes = new Set<number>();
  for (const fn of golden.program.program.functions) {
    for (const ins of fn.code) {
      opcodes.add(ins.op);
    }
  }
  return opcodes;
}

/** Runs a fixture binary over its schedule and renders the trace. */
function runFixtureTrace(fixture: TriggerFixture, bin: Uint8Array): string {
  const environment = createMicroBitV2Environment();
  const profile = getWodalDeviceProfile(WodalDeviceProfileId.MICROBIT_V2);
  const decoded = parseWodalProgramImageBytes(
    bin,
    WodalDeviceProfileId.MICROBIT_V2,
    environment.brainServices.runtime.types
  );
  const writer = new ObservableTraceWriter({
    profileId: profile.numericProfileId,
    precision: profile.numberPrecision,
  });

  const microbit = new MicroBit();
  const deviceSetPixelValue = microbit.display.setPixelValue.bind(microbit.display);
  microbit.display.setPixelValue = (x, y, brightness) => {
    writer.displaySetPixel(x, y, brightness);
    deviceSetPixelValue(x, y, brightness);
  };
  const deviceScrollText = microbit.display.scrollText.bind(microbit.display);
  microbit.display.scrollText = (text, durationMs, requestTime, onComplete) => {
    if (!microbit.display.isBusy()) {
      writer.displayScroll(text);
    }
    deviceScrollText(text, durationMs, requestTime, onComplete);
  };

  const vmEvents = observableTraceVmEvents(writer);
  const runtime = new WodalMicroBitRuntime({ environment, microbit, vmEvents });
  assert.deepEqual(runtime.loadWodalProgramImage(profile.createProgramImage(decoded.program)), { ok: true });

  let lastThinkTimeMs = 0;
  for (const [index, step] of fixture.schedule.entries()) {
    if (step.a !== undefined) {
      microbit.setButtonPressed("A", step.a);
    }
    if (step.b !== undefined) {
      microbit.setButtonPressed("B", step.b);
    }
    for (const packet of step.inject ?? []) {
      microbit.radio.deliver(packet);
    }
    const timeMs = lastThinkTimeMs + step.advanceMs;
    writer.tick(index + 1, timeMs, lastThinkTimeMs === 0 ? 0 : timeMs - lastThinkTimeMs);
    runtime.tick(step.advanceMs);
    microbit.display.advanceScroll(timeMs);
    lastThinkTimeMs = timeMs;
  }
  return writer.render();
}

/** The IEEE-754 f32 hex the trace renders `value` as. */
function f32Hex(value: number): string {
  const view = new DataView(new ArrayBuffer(4));
  view.setFloat32(0, value, false);
  return view.getUint32(0, false).toString(16).padStart(8, "0");
}

/** The `[x, y]` marker pixels of each think, in trace order, one entry per tick. */
function pixelsPerTick(trace: string, tickCount: number): string[][] {
  const perTick: string[][] = [];
  for (let i = 0; i < tickCount; i++) {
    perTick.push([]);
  }
  let currentTick = 0;
  for (const line of trace.split("\n")) {
    if (line.startsWith("tick ")) {
      currentTick = Number.parseInt(line.split(" ")[1]!, 16);
    } else if (line.startsWith("port display set-pixel ")) {
      const parts = line.split(" ");
      perTick[currentTick - 1]!.push(`${parts[3]} ${parts[4]}`);
    }
  }
  return perTick;
}

/** The rule-trigger host-action dispatch count of each think, one entry per tick. */
function triggerDispatchesPerTick(trace: string, tickCount: number): number[] {
  const perTick: number[] = [];
  for (let i = 0; i < tickCount; i++) {
    perTick.push(0);
  }
  let currentTick = 0;
  for (const line of trace.split("\n")) {
    if (line.startsWith("tick ")) {
      currentTick = Number.parseInt(line.split(" ")[1]!, 16);
    } else if (line.startsWith(TRIGGER_LINE_PREFIX)) {
      perTick[currentTick - 1]! += 1;
    }
  }
  return perTick;
}

/** The display-scroll dispatch count of each think, one entry per tick. */
function scrollDispatchesPerTick(trace: string, tickCount: number): number[] {
  const perTick: number[] = [];
  for (let i = 0; i < tickCount; i++) {
    perTick.push(0);
  }
  let currentTick = 0;
  for (const line of trace.split("\n")) {
    if (line.startsWith("tick ")) {
      currentTick = Number.parseInt(line.split(" ")[1]!, 16);
    } else if (line.startsWith("port display scroll ")) {
      perTick[currentTick - 1]! += 1;
    }
  }
  return perTick;
}

/** The expected per-tick marker pixels rendered in the trace's hex form. */
function expectedPixelsAsHex(fixture: TriggerFixture): string[][] {
  return fixture.expectedPixels.map((tick) => tick.map(([x, y]) => `${f32Hex(x)} ${f32Hex(y)}`));
}

for (const fixture of FIXTURES) {
  test(`the committed ${fixture.name} binary and observable trace golden are byte-stable`, () => {
    assert.equal(
      fixture.expectedPixels.length,
      fixture.schedule.length,
      "every scheduled think needs its expected markers"
    );
    const jsonPath = fixturePath(fixture.name, "mcprogram");
    const binPath = fixturePath(fixture.name, "mcprogram.bin");
    const tracePath = fixturePath(fixture.name, "ticks.trace");

    ensureJsonGolden(jsonPath, fixture);
    const generated = wodalProgramBytes(new Uint8Array(readFileSync(jsonPath)));
    if (shouldWriteGolden(binPath)) {
      writeFileSync(binPath, generated);
    }
    const bin = new Uint8Array(readFileSync(binPath));
    assert.deepEqual(bin, generated, `${fixture.name}.mcprogram.bin is not byte-stable`);

    const opcodes = committedOpcodes(jsonPath);
    for (const opcode of fixture.requiredOpcodes ?? []) {
      assert.ok(opcodes.has(opcode), `the compiled bytecode should carry opcode ${opcode}`);
    }

    const first = runFixtureTrace(fixture, bin);
    const second = runFixtureTrace(fixture, bin);
    assert.equal(second, first, "two fresh runs must render byte-identical traces");

    const lines = first.split("\n");
    assert.equal(lines.filter((line) => line.startsWith("tick ")).length, fixture.schedule.length);
    assert.equal(lines.filter((line) => line.startsWith("fault ")).length, 0);

    assert.deepEqual(pixelsPerTick(first, fixture.schedule.length), expectedPixelsAsHex(fixture));

    if (fixture.expectedScrolls !== undefined) {
      assert.deepEqual(
        scrollDispatchesPerTick(first, fixture.schedule.length),
        [...fixture.expectedScrolls],
        "every link of the chain scrolls, in document order"
      );
    }
    if (fixture.expectedTriggerDispatches !== undefined) {
      assert.deepEqual(
        triggerDispatchesPerTick(first, fixture.schedule.length),
        [...fixture.expectedTriggerDispatches],
        "the rule-trigger dispatches of each think separate a waiting rule from a skipping one"
      );
    }

    if (shouldWriteGolden(tracePath)) {
      writeFileSync(tracePath, first);
    }
    assert.equal(readFileSync(tracePath, "utf8"), first, `${fixture.name}.ticks.trace is not byte-stable`);
  });
}
