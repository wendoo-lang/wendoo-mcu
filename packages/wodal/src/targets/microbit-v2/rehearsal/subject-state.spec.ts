import assert from "node:assert/strict";
import { describe, test } from "node:test";
import type { AuthoringWorkspace, SimulationRun } from "@wendoo/assistant-bridge";
import { createAuthoringWorkspace, proposeEdit } from "@wendoo/assistant-bridge";
import { FAKE_TARGET_IDENTITY, ruleIdAt } from "@wendoo/assistant-bridge/testing";
import { mkActuatorTileId, mkParameterTileId } from "@wendoo/core/app";
import { CoreLiteralFactoryId, mkLiteralFactoryTileId } from "@wendoo/core/brain";
import { builtInSoundTileId, findBuiltInSound } from "../wendoo/built-in-sounds";
import { MicroBitV2HostActions, WodalMicroBitV2ParameterId } from "../wendoo/tile-ids";
import { createTargetAdapter } from "./adapter";

/** The one role a scenario may put under study. */
const SUBJECT = "device";

/** A blank screen, as the display channel renders it. */
const BLANK = "0".repeat(25);

/** One tile of a rule side, by id or as a factory naming what to mint. */
type Placement = string | { tileId: string; value: number | string };

/** Number literal `value`, as a rule side carries it. */
function number(value: number): Placement {
  return { tileId: mkLiteralFactoryTileId(CoreLiteralFactoryId.Number), value };
}

/** A brain of one unconditional rule that does `does`, over a fresh workspace on this target. */
function workspaceDoing(does: readonly Placement[]): AuthoringWorkspace {
  const workspace = createAuthoringWorkspace(createTargetAdapter(FAKE_TARGET_IDENTITY), "state brain");
  const ruleId = ruleIdAt(workspace.brainDef, "0/0");
  const result = proposeEdit(workspace, { op: "placeTiles", ruleId, side: "do", tileIds: [...does] });
  assert.equal(result.ok, true, JSON.stringify(result));
  return workspace;
}

/** Rehearse `workspace` for `thinks` thinks over a scenario scripting nothing. */
function rehearse(workspace: AuthoringWorkspace, thinks: number): Promise<SimulationRun> {
  return workspace.adapter.run({
    brainDef: workspace.brainDef,
    scenario: { seed: 20260812, subject: SUBJECT },
    thinks,
  });
}

/** Every state entry the run recorded for `channel`, as `think value`. */
function changesOf(run: SimulationRun, channel: string): string[] {
  const marker = `${channel}=`;
  return run.observations.flatMap((think, index) =>
    (think.state ?? [])
      .filter((entry) => entry.startsWith(marker))
      .map((entry) => `${index} ${entry.slice(marker.length)}`)
  );
}

/** Light the pixel at column `x`, row `y`. */
function setPixel(x: number, y: number): readonly Placement[] {
  return [
    mkActuatorTileId(MicroBitV2HostActions.DisplaySetPixel.key),
    mkParameterTileId(WodalMicroBitV2ParameterId.X),
    number(x),
    mkParameterTileId(WodalMicroBitV2ParameterId.Y),
    number(y),
  ];
}

describe("the state channels this target declares", () => {
  test("names the screen and the speaker, each with a sentence saying how to read it", () => {
    const channels = createTargetAdapter(FAKE_TARGET_IDENTITY).stateChannels();

    assert.deepEqual(
      channels.map((channel) => channel.name),
      ["display", "speaker"]
    );
    for (const channel of channels) assert.ok(channel.description.trim().length > 0);
  });

  test("leaves the speaker standing at the sound itself, however often that sound has started", () => {
    const speaker = createTargetAdapter(FAKE_TARGET_IDENTITY)
      .stateChannels()
      .find((channel) => channel.name === "speaker");

    assert.ok(speaker?.identityValue, "the speaker says what it contributes");
    assert.equal(speaker.identityValue("giggle#1"), speaker.identityValue("giggle#7"));
    assert.notEqual(speaker.identityValue("giggle#1"), speaker.identityValue("hello#1"));
    assert.notEqual(speaker.identityValue("giggle#1"), speaker.identityValue("idle"));
  });

  test("leaves the screen standing at what it shows, which it reports whole", () => {
    const display = createTargetAdapter(FAKE_TARGET_IDENTITY)
      .stateChannels()
      .find((channel) => channel.name === "display");

    assert.equal(display?.identityValue, undefined);
  });
});

describe("what a rehearsal records of the screen", () => {
  test("records the lit pixel in its row-major place, at full brightness", async () => {
    const run = await rehearse(workspaceDoing(setPixel(2, 2)), 4);

    const [first] = changesOf(run, "display");
    assert.equal(first, `0 ${"0".repeat(12)}9${"0".repeat(12)}`, "the middle pixel is the thirteenth digit");
  });

  test("reports the screen once and then holds quiet while it does not change", async () => {
    const run = await rehearse(workspaceDoing(setPixel(4, 4)), 12);

    assert.equal(changesOf(run, "display").length, 1, "a still screen reports only its opening value");
  });

  test("keeps a screen no rule writes at its blank baseline", async () => {
    const workspace = createAuthoringWorkspace(createTargetAdapter(FAKE_TARGET_IDENTITY), "idle state brain");

    const run = await rehearse(workspace, 6);

    assert.deepEqual(changesOf(run, "display"), [`0 ${BLANK}`]);
  });
});

describe("what a rehearsal records of the speaker", () => {
  test("names the sound playing, and tells one play of it from the next by its number", async () => {
    const sound = findBuiltInSound("hello")!;
    const workspace = workspaceDoing([
      mkActuatorTileId(MicroBitV2HostActions.PlaySound.key),
      builtInSoundTileId(sound),
    ]);

    const run = await rehearse(workspace, 80);

    const heard = changesOf(run, "speaker").map((entry) => entry.split(" ")[1]!);
    assert.equal(heard[0], `${sound.name}#1`, "the play the first think starts is audible when that think closes");
    assert.ok(
      heard.includes(`${sound.name}#2`),
      `a second play of the same sound reads apart from the first: ${heard.join("; ")}`
    );
  });

  test("keeps a speaker no rule drives idle for the whole run", async () => {
    const run = await rehearse(workspaceDoing(setPixel(0, 0)), 8);

    assert.deepEqual(changesOf(run, "speaker"), ["0 idle"]);
  });
});
