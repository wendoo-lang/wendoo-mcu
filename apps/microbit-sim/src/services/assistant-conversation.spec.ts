/**
 * Drives this app's own assistant composition -- the manifest it declares, the
 * workspaces the editor stands, and the session machine the provider wraps --
 * through whole conversations against a scripted service. Reads the result out
 * of the conversation record and out of the editor's document.
 */

import assert from "node:assert/strict";
import { describe, test } from "node:test";
import { assistantToolManifest } from "@wendoo/assistant-bridge/relay";
import { ruleIdAt } from "@wendoo/assistant-bridge/testing";
import type { EditedBrainWorkspaces, PersonActivity } from "@wendoo/assistant-panel";
import { createEditedBrainWorkspaces, createPersonActivity } from "@wendoo/assistant-panel";
import { recordFor } from "@wendoo/assistant-panel/conversation/store";
import type { AssistantChannel } from "@wendoo/assistant-panel/session/channel";
import { AssistantMachine, AssistantStatus } from "@wendoo/assistant-panel/session/machine";
import type { ScriptedService } from "@wendoo/assistant-panel/testing/scripted-service";
import { runScriptedService } from "@wendoo/assistant-panel/testing/scripted-service";
import type { ConversationAssistantEntry, ConversationRecord, ConversationToolCall } from "@wendoo/assistant-relay";
import type { RelayLoopback } from "@wendoo/assistant-relay/testing";
import { createRelayLoopback } from "@wendoo/assistant-relay/testing";
import { __test__clientBuild } from "@wendoo/core/__test__";
import { List } from "@wendoo/core/app";
import type { BrainPageDef, BrainRuleDef } from "@wendoo/core/brain/model";
import { BrainCommandHistory, BrainDef } from "@wendoo/core/brain/model";
import type { EditedBrain } from "@wendoo/ui";
import { createMicroBitV2Environment } from "@wendoo/wodal/targets/microbit-v2";
import { createTargetAdapter } from "@wendoo/wodal/targets/microbit-v2/rehearsal";
import { MICROBIT_V2_TARGET_COORDINATE } from "./microbit-extension-coordinates";

/** The WHEN tiles one scripted turn authors: button A going down. */
const authoredWhenTiles = ["tile.sensor->microbit-v2.button-a", "tile.modifier->microbit-v2.pressed"] as const;

/** The DO tiles one scripted turn authors: showing a happy face. */
const authoredDoTiles = ["tile.actuator->microbit-v2.draw-image", "tile.literal->struct:<Image>->happy"] as const;

/** Undoable history entries the authoring turn leaves, one per accepted run. */
const authoredEdits = 2;

/** The propose_edit calls the authoring turn makes against the rule `ruleId` names. */
function authoringCalls(ruleId: string) {
  return [
    { name: "propose_edit", input: { op: "placeTiles", ruleId, side: "when", tileIds: [...authoredWhenTiles] } },
    { name: "propose_edit", input: { op: "placeTiles", ruleId, side: "do", tileIds: [...authoredDoTiles] } },
  ];
}

/** One turn that looks at the catalog, authors the rule `ruleId` names, and finishes. */
function authoringTurn(ruleId: string): ScriptedService {
  return {
    turns: [
      {
        steps: [
          { kind: "narration", text: "I will watch button A first." },
          { kind: "toolCalls", calls: [{ name: "read_catalog", input: {} }] },
          { kind: "toolCalls", calls: authoringCalls(ruleId) },
          { kind: "narration", text: "Now I show a happy face." },
        ],
      },
    ],
  };
}

/** One turn that narrates and then waits to be stopped. */
function haltingTurn(): ScriptedService {
  return { turns: [{ steps: [{ kind: "narration", text: "Thinking about it." }, { kind: "awaitStop" }] }] };
}

/** The app's composition under test, and the sessions it opened. */
interface Stand {
  readonly machine: AssistantMachine;
  readonly workspaces: EditedBrainWorkspaces;
  /** Where the person's own acting on the edited brain is recorded, shared by the machine and the workspaces. */
  readonly activity: PersonActivity;
  /** Opens a working copy of a brain named `name`, as the editor does. */
  editorOpenedOn(name: string): EditedBrain;
  /** How many sessions the machine has asked for. */
  connects(): number;
  /** Resolves once every scripted service has played out. */
  settled(): Promise<void>;
  record(brainId: string): ConversationRecord;
}

/**
 * Stand the app's composition with every session answered by the script
 * `script` builds for the rule the edited document opens with. Each session
 * gets its own script, so a turn addresses the rule of the brain it is for.
 */
function appStand(script: (ruleId: string) => ScriptedService): Stand {
  const environment = createMicroBitV2Environment();
  const adapter = createTargetAdapter(MICROBIT_V2_TARGET_COORDINATE);
  const activity = createPersonActivity();
  const workspaces = createEditedBrainWorkspaces({ environment, adapter, activity });
  const services: Promise<void>[] = [];
  let connects = 0;

  const connect = (): Promise<AssistantChannel> => {
    connects++;
    const loopback: RelayLoopback = createRelayLoopback();
    const activeBrainId = machine.getState().store.activeBrainId!;
    const editedRuleId = ruleIdAt(workspaces.workspaceFor(activeBrainId).brainDef, "0/0");
    services.push(runScriptedService(loopback, script(editedRuleId)));
    return Promise.resolve({
      send: (message) => loopback.toolServer.send(message),
      next: () => loopback.toolServer.next(),
      close: () => loopback.toolServer.close(),
      closed: loopback.toolServer.closed,
    });
  };

  const machine = new AssistantMachine({
    connect,
    manifest: assistantToolManifest(adapter),
    clientBuild: __test__clientBuild,
    workspace: workspaces.workspaceFor,
    activity,
  });

  return {
    machine,
    workspaces,
    activity,
    editorOpenedOn: (name) => ({
      brainDef: BrainDef.emptyBrainDef(environment.brainServices, name).workingCopy(
        List.from(environment.tileCatalogs())
      ),
      history: new BrainCommandHistory(),
      reveal: () => {},
      takeKeyboard: () => false,
    }),
    connects: () => connects,
    settled: async () => {
      await Promise.all(services);
    },
    record: (brainId) => recordFor(machine.getState().store, brainId),
  };
}

/** The tile ids on `side` of the document's first rule. */
function ruleSideTileIds(brainDef: BrainDef, side: "when" | "do"): string[] {
  const page = brainDef.pages().get(0) as BrainPageDef;
  const rule = page.children().get(0) as BrainRuleDef;
  return (side === "when" ? rule.when() : rule.do())
    .tiles()
    .toArray()
    .map((tile) => tile.tileId);
}

/** The one assistant turn in `record`. */
function onlyTurn(record: ConversationRecord): ConversationAssistantEntry {
  const turns = record.entries.filter((entry) => entry.kind === "assistant");
  assert.equal(turns.length, 1, "the record holds one turn");
  return turns[0] as ConversationAssistantEntry;
}

/** The calls `turn` made, in the order it made them. */
function callsOf(turn: ConversationAssistantEntry): ConversationToolCall[] {
  return turn.steps.flatMap((step) => (step.kind === "toolCall" ? [step.call] : []));
}

/** Resolves once `predicate` holds of the machine's state, or fails. */
async function settle(stand: Stand, predicate: () => boolean): Promise<void> {
  for (let waited = 0; waited < 400; waited++) {
    if (predicate()) return;
    await new Promise((resolve) => setTimeout(resolve, 5));
  }
  throw new Error("the machine never reached the state the test waited for");
}

describe("a whole conversation over this app's composition", () => {
  test("lands the turn's tiles in the editor's document and records what it did", async () => {
    const stand = appStand(authoringTurn);
    const edited = stand.editorOpenedOn("Greeter");
    const brainId = edited.brainDef.id();
    stand.workspaces.setEditedBrain(edited);
    stand.machine.setActiveBrain(brainId);

    stand.machine.send("show a happy face when I press A");
    await settle(stand, () => stand.machine.getState().status !== AssistantStatus.TurnActive);
    await stand.settled();

    const record = stand.record(brainId);
    assert.deepEqual(record.entries[0], { kind: "user", text: "show a happy face when I press A" });
    const turn = onlyTurn(record);
    assert.deepEqual(turn.ending, { kind: "end", code: "complete" });
    assert.deepEqual(
      callsOf(turn).map((call) => call.name),
      ["read_catalog", "propose_edit", "propose_edit"]
    );
    assert.deepEqual(
      callsOf(turn).map((call) => call.outcome.kind),
      ["ok", "ok", "ok"]
    );
    assert.deepEqual(
      turn.steps.map((step) => step.kind),
      ["narration", "toolCall", "toolCall", "toolCall", "narration"],
      "the turn's narration and calls stand in the order they arrived"
    );
    assert.deepEqual(ruleSideTileIds(edited.brainDef, "when"), [...authoredWhenTiles]);
    assert.deepEqual(ruleSideTileIds(edited.brainDef, "do"), [...authoredDoTiles]);
    assert.equal(edited.history.undoDepth(), authoredEdits);
  });

  test("ends the turn the person stopped", async () => {
    const stand = appStand(haltingTurn);
    const edited = stand.editorOpenedOn("Greeter");
    const brainId = edited.brainDef.id();
    stand.workspaces.setEditedBrain(edited);
    stand.machine.setActiveBrain(brainId);

    stand.machine.send("keep going forever");
    await settle(stand, () => stand.record(brainId).entries.length > 1);
    stand.machine.stop();
    await settle(stand, () => stand.machine.getState().status !== AssistantStatus.TurnActive);
    await stand.settled();

    assert.deepEqual(onlyTurn(stand.record(brainId)).ending, { kind: "end", code: "stopped" });
  });
});

describe("two brains edited in turn", () => {
  test("keeps each brain's conversation to itself and shows the one the editor stands", async () => {
    const stand = appStand(authoringTurn);
    const greeter = stand.editorOpenedOn("Greeter");
    const counter = stand.editorOpenedOn("Counter");

    stand.workspaces.setEditedBrain(greeter);
    stand.machine.setActiveBrain(greeter.brainDef.id());
    stand.machine.send("show a happy face when I press A");
    await settle(stand, () => stand.machine.getState().status !== AssistantStatus.TurnActive);

    stand.workspaces.setEditedBrain(counter);
    stand.machine.setActiveBrain(counter.brainDef.id());
    stand.machine.send("show a happy face when I press A");
    await settle(stand, () => stand.machine.getState().status !== AssistantStatus.TurnActive);
    await stand.settled();

    const shown = stand.machine.getState().store;
    assert.equal(shown.activeBrainId, counter.brainDef.id());
    assert.deepEqual(
      stand.record(greeter.brainDef.id()).entries.map((entry) => entry.kind),
      ["user", "assistant"]
    );
    assert.deepEqual(
      stand.record(counter.brainDef.id()).entries.map((entry) => entry.kind),
      ["user", "assistant"]
    );
    assert.equal(stand.connects(), 2, "each brain opened its own session");
    assert.deepEqual(ruleSideTileIds(greeter.brainDef, "when"), [...authoredWhenTiles]);
    assert.deepEqual(ruleSideTileIds(counter.brainDef, "when"), [...authoredWhenTiles]);
  });
});

describe("an editor closed and opened again on the same brain", () => {
  test("reuses the session the first open stood", async () => {
    const stand = appStand(authoringTurn);
    const edited = stand.editorOpenedOn("Greeter");
    const brainId = edited.brainDef.id();

    stand.workspaces.setEditedBrain(edited);
    stand.machine.setActiveBrain(brainId);
    stand.machine.openSession(brainId);
    await settle(stand, () => stand.machine.getState().status === AssistantStatus.Ready);

    stand.workspaces.setEditedBrain(undefined);
    stand.workspaces.setEditedBrain(edited);
    stand.machine.openSession(brainId);
    stand.machine.send("show a happy face when I press A");
    await settle(stand, () => stand.machine.getState().status !== AssistantStatus.TurnActive);
    await stand.settled();

    assert.equal(stand.connects(), 1, "the brain's session outlived the editor");
    assert.deepEqual(onlyTurn(stand.record(brainId)).ending, { kind: "end", code: "complete" });
  });
});

describe("the person's own hands on the brain being edited", () => {
  test("records nothing for a whole turn's edits, so the entity never takes itself over", async () => {
    const stand = appStand(authoringTurn);
    const edited = stand.editorOpenedOn("Greeter");
    const brainId = edited.brainDef.id();
    stand.workspaces.setEditedBrain(edited);
    stand.machine.setActiveBrain(brainId);

    stand.machine.send("show a happy face when I press A");
    await settle(stand, () => stand.machine.getState().status !== AssistantStatus.TurnActive);
    await stand.settled();

    assert.deepEqual(onlyTurn(stand.record(brainId)).ending, { kind: "end", code: "complete" });
    assert.equal(edited.history.undoDepth(), authoredEdits, "the turn's edits really reached the history");
    assert.equal(stand.activity.mutations(brainId), 0);
  });

  test("takes the turn over at the next call, keeping every edit that already landed", async () => {
    const stand = appStand(authoringTurn);
    const opened = stand.editorOpenedOn("Greeter");
    const brainId = opened.brainDef.id();
    // The person's hands land the moment the batch's first edit does.
    let noted = false;
    const edited: EditedBrain = {
      ...opened,
      reveal: () => {
        if (noted) return;
        noted = true;
        stand.activity.noteMutation(brainId);
      },
    };
    stand.workspaces.setEditedBrain(edited);
    stand.machine.setActiveBrain(brainId);

    stand.machine.send("show a happy face when I press A");
    await settle(stand, () => stand.machine.getState().status !== AssistantStatus.TurnActive);
    await stand.settled();

    const turn = onlyTurn(stand.record(brainId));
    assert.deepEqual(turn.ending, { kind: "end", code: "stopped" });
    assert.deepEqual(
      callsOf(turn).map((call) => call.outcome.kind),
      ["ok", "ok", "takeover"]
    );
    assert.deepEqual(ruleSideTileIds(edited.brainDef, "when"), [...authoredWhenTiles], "the landed edit stays");
    assert.deepEqual(ruleSideTileIds(edited.brainDef, "do"), [], "the call the person took over ran nothing");
    assert.equal(stand.machine.getState().status, AssistantStatus.Ready, "the session stays standing");
  });

  test("leaves the view alone while the person is working the rules", async () => {
    const stand = appStand(authoringTurn);
    const opened = stand.editorOpenedOn("Greeter");
    const brainId = opened.brainDef.id();
    const revealed: unknown[] = [];
    const edited: EditedBrain = { ...opened, reveal: (place) => revealed.push(place) };
    stand.workspaces.setEditedBrain(edited);
    stand.machine.setActiveBrain(brainId);
    stand.activity.noteInteraction(brainId);

    stand.machine.send("show a happy face when I press A");
    await settle(stand, () => stand.machine.getState().status !== AssistantStatus.TurnActive);
    await stand.settled();

    assert.equal(edited.history.undoDepth(), authoredEdits, "the edits landed all the same");
    assert.deepEqual(revealed, []);
  });
});
