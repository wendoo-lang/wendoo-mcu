/**
 * Verifies that a brain run through {@link WodalMicroBitRuntime} honors the
 * device profile's concurrent async-handle cap, so the simulator reproduces the
 * hardware's backpressure timing.
 *
 * The runtime sources its handle cap from `deviceProfile.maxHandles`. A brain
 * whose same-think async breadth exceeds that cap spills its async dispatches
 * across thinks in bounded waves whose first wave equals the profile cap; a brain
 * whose breadth is within the cap dispatches everything in a single think.
 *
 * The fixtures here are independent sibling rules over hosts that settle, so
 * every backpressured dispatch clears within a few rounds. A dispatch starved
 * for `HANDLE_BACKPRESSURE_FAULT_ROUNDS` consecutive rounds instead faults
 * `StackOverflow`; that path is pinned in the core VM specs.
 */

import assert from "node:assert/strict";
import { test } from "node:test";
import {
  BrainTileLiteralDef,
  CoreHostActions,
  CoreTypeIds,
  mkActuatorTileId,
  mkParameterTileId,
  mkSensorTileId,
  type WendooEnvironment,
} from "@wendoo/core/app";
import { BrainDef, type BrainRuleDef } from "@wendoo/core/brain/model";
import type { LinkedBrainProgram } from "@wendoo/core/runtime";
import { buildWodalProgramImage } from "../../../wendoo/build-kernel";
import { getWodalDeviceProfile, WodalDeviceProfileId } from "../../../wendoo/device-profile";
import type { WodalProgramImage } from "../../../wendoo/program-image";
import { MicroBit } from "../microbit";
import { createMicroBitV2Environment } from "./environment";
import { WodalMicroBitRuntime } from "./runtime";
import { MicroBitV2HostActions, WodalMicroBitV2ParameterId } from "./tile-ids";

const DISPLAY_SCROLL_KEY = MicroBitV2HostActions.DisplayScroll.key;
const TICK_ADVANCE_MS = 1100;
const TICK_COUNT = 12;

/**
 * A one-page brain: a parent rule (WHEN on-page-entered) that lights pixel x=0,
 * with `childCount` child rules that each scroll text asynchronously. Child rules
 * drain synchronously in the parent's think, so all of them attempt their async
 * scroll dispatch in one think -- a same-think async breadth of `childCount`.
 */
function buildBrainDef(env: WendooEnvironment, childCount: number): BrainDef {
  const tiles = env.brainServices.edit.tiles;
  const onPageEntered = tiles.get(mkSensorTileId(CoreHostActions.OnPageEntered.key));
  const setPixel = tiles.get(mkActuatorTileId(MicroBitV2HostActions.DisplaySetPixel.key));
  const xParam = tiles.get(mkParameterTileId(WodalMicroBitV2ParameterId.X));
  const scrollTile = tiles.get(mkActuatorTileId(MicroBitV2HostActions.DisplayScroll.key));
  assert.ok(onPageEntered);
  assert.ok(setPixel);
  assert.ok(xParam);
  assert.ok(scrollTile);

  const brainDef = BrainDef.emptyBrainDef(env.brainServices, "handle-cap sim brain");
  const parent = brainDef.pages().get(0)!.children().get(0)! as BrainRuleDef;
  parent.when().appendTile(onPageEntered);
  parent.do().appendTile(setPixel);
  parent.do().appendTile(xParam);
  const xLiteral = new BrainTileLiteralDef(CoreTypeIds.Number, 0, {}, env.brainServices);
  brainDef.catalog().registerTileDef(xLiteral);
  parent.do().appendTile(xLiteral);

  for (let i = 0; i < childCount; i++) {
    const child = parent.appendNewRule();
    child.do().appendTile(scrollTile);
  }

  return brainDef;
}

function buildImage(env: WendooEnvironment, childCount: number): WodalProgramImage<LinkedBrainProgram> {
  const built = buildWodalProgramImage({
    brainDef: buildBrainDef(env, childCount),
    environment: env,
    deviceProfile: getWodalDeviceProfile(WodalDeviceProfileId.MICROBIT_V2),
  });
  if (!built.ok) {
    assert.fail(`expected a successful build: ${JSON.stringify(built.errors)}`);
  }
  return built.image;
}

/**
 * Loads a brain with `childCount` async-scroll child rules into a fresh runtime,
 * ticks it, and returns the count of scroll dispatches observed in each think, in
 * order (thinks with no dispatch omitted). A fault flips `faulted` true.
 */
function runWaves(childCount: number): { waves: number[]; faulted: boolean } {
  const environment = createMicroBitV2Environment();

  let faulted = false;
  const perTick: number[] = [];

  const microbit = new MicroBit();
  const runtime = new WodalMicroBitRuntime({
    environment,
    microbit,
    vmEvents: {
      onFiberFault: () => {
        faulted = true;
      },
      onHostActionDispatch: (payload) => {
        if (payload.descriptor.key === DISPLAY_SCROLL_KEY) {
          perTick[perTick.length - 1]!++;
        }
      },
    },
  });
  assert.deepEqual(runtime.loadWodalProgramImage(buildImage(environment, childCount)), { ok: true });

  for (let i = 0; i < TICK_COUNT; i++) {
    perTick.push(0);
    runtime.tick(TICK_ADVANCE_MS);
  }

  return { waves: perTick.filter((count) => count > 0), faulted };
}

test("a brain whose async breadth exceeds the profile handle cap wave-spreads across thinks under the runtime", () => {
  const cap = getWodalDeviceProfile(WodalDeviceProfileId.MICROBIT_V2).maxHandles;
  const { waves, faulted } = runWaves(cap + 2);

  assert.equal(
    faulted,
    false,
    "independent sibling breadth over settling hosts parks and retries within a few rounds, never long enough to fault"
  );
  assert.equal(
    waves.reduce((sum, count) => sum + count, 0),
    cap + 2,
    "every child rule's async scroll eventually dispatches -- none lost to the cap"
  );
  assert.ok(waves.length > 1, "the async breadth spills across more than one think");
  assert.equal(waves[0], cap, "the first wave fills exactly the profile's handle cap");
  for (const count of waves) {
    assert.ok(count <= cap, "no wave dispatches more concurrent async than the profile cap");
  }
});

test("the runtime's effective handle cap equals the device profile's maxHandles", () => {
  // With breadth just past the cap, the first-wave dispatch count is the runtime's
  // effective concurrent-handle cap. Anchoring it to the profile field proves the
  // runtime sources the cap from the profile, so a future profile change flows
  // through without re-wiring the runtime.
  const cap = getWodalDeviceProfile(WodalDeviceProfileId.MICROBIT_V2).maxHandles;
  const { waves } = runWaves(cap + 1);
  assert.equal(waves[0], cap, "first-wave breadth (the effective cap) tracks deviceProfile.maxHandles");
});

test("a brain whose async breadth is within the profile handle cap dispatches in a single think", () => {
  const cap = getWodalDeviceProfile(WodalDeviceProfileId.MICROBIT_V2).maxHandles;
  const { waves, faulted } = runWaves(cap);

  assert.equal(faulted, false, "a within-cap breadth never faults");
  assert.deepEqual(waves, [cap], "a within-cap async breadth dispatches everything in one think");
});
