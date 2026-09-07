/**
 * What the assistant sees of an image a user drew: the literal the image
 * factory mints into the document's own catalog reads by its name in
 * read_catalog and read_project, is found by a filter on that name, keeps one
 * tile id and one placement across an edit of its pixels, and names the image
 * in the arguments a rehearsal records for the draw it dispatched.
 */

import assert from "node:assert/strict";
import { describe, test } from "node:test";
import type { AuthoringWorkspace, CatalogTile, ProjectTile, SimulationRun } from "@wendoo/assistant-bridge";
import {
  CatalogScope,
  catalogTiles,
  catalogTilesInScope,
  createAuthoringWorkspace,
  proposeEdit,
  readCatalog,
  readProject,
} from "@wendoo/assistant-bridge";
import { FAKE_TARGET_IDENTITY, ruleIdAt } from "@wendoo/assistant-bridge/testing";
import { mkActuatorTileId, mkSensorTileId } from "@wendoo/core/app";
import { mkLiteralFactoryTileId } from "@wendoo/core/brain";
import { EditLiteralCommand } from "@wendoo/core/brain/model";
import type { BrainTileFactoryDef, BrainTileLiteralDef } from "@wendoo/core/brain/tiles";
import { manufactureLiteralTile } from "@wendoo/core/brain/tiles";
import { WODAL_IMAGE_LITERAL_FACTORY_ID } from "@wendoo/wodal";
import { MicroBitV2HostActions } from "@wendoo/wodal/targets/microbit-v2";
import { createTargetAdapter } from "@wendoo/wodal/targets/microbit-v2/rehearsal";
import { imageGridBytes, imageLiteralType, kImagePixelsFieldKey } from "../brain/image-literal-type";

/** The one role this target puts under study. */
const SUBJECT = "device";

/**
 * The drawn grid the image starts as: one hex brightness digit per pixel,
 * row-major. No built-in icon carries these pixels, so no environment literal
 * names this value.
 */
const kDrawnDigits = ["12345", "6789a", "bcdef", "13579", "2468a"].join("");

/** The drawn grid the same image is edited to. */
const kEditedDigits = ["2468a", "13579", "bcdef", "6789a", "12345"].join("");

/** The word the user gives the drawn image. */
const kImageName = "rock";

/** The `Image` struct value the grid editor produces for `digits`. */
function imageValue(digits: string): unknown {
  const value = imageLiteralType.parseValue({ [kImagePixelsFieldKey]: digits });
  assert.ok(value !== undefined, "the grid editor parses the digits it produced");
  return value;
}

/** The brightness byte per pixel `digits` stands for, row-major. */
function expectedBytes(digits: string): number[] {
  return [...digits].map((digit) => Number.parseInt(digit, 16) * 17);
}

/** Place `tileIds` on `side` of `ruleId`, asserting the editor accepted the run. */
function place(workspace: AuthoringWorkspace, ruleId: string, side: "when" | "do", tileIds: string[]): void {
  const result = proposeEdit(workspace, { op: "placeTiles", ruleId, side, tileIds });
  assert.equal(result.ok, true, JSON.stringify(result));
}

/** The image literal factory this target's environment registers. */
function imageFactory(workspace: AuthoringWorkspace): BrainTileFactoryDef {
  const factory = workspace.environment.brainServices.edit.tiles.get(
    mkLiteralFactoryTileId(WODAL_IMAGE_LITERAL_FACTORY_ID)
  );
  assert.ok(factory, "the image literal factory is registered");
  return factory as BrainTileFactoryDef;
}

/**
 * A session whose one rule draws a named image the user drew, minted through
 * the image factory into the document's own catalog.
 */
function drawnImageSession(): { workspace: AuthoringWorkspace; literal: BrainTileLiteralDef; ruleId: string } {
  const workspace = createAuthoringWorkspace(createTargetAdapter(FAKE_TARGET_IDENTITY), "drawn image brain");
  const literal = manufactureLiteralTile(
    imageFactory(workspace),
    workspace.brainDef.catalog(),
    imageValue(kDrawnDigits),
    undefined,
    kImageName
  );
  assert.ok(literal, "the image factory mints a literal");
  const ruleId = ruleIdAt(workspace.brainDef, "0/0");
  place(workspace, ruleId, "when", [mkSensorTileId(MicroBitV2HostActions.ButtonA.key)]);
  place(workspace, ruleId, "do", [mkActuatorTileId(MicroBitV2HostActions.DrawImage.key), literal.tileId]);
  return { workspace, literal, ruleId };
}

/** The tiles `read_project` reports on the DO side of `ruleId`. */
function projectDoTiles(workspace: AuthoringWorkspace, ruleId: string): readonly ProjectTile[] {
  const rule = readProject(workspace)
    .pages.flatMap((page) => page.rules)
    .find((entry) => entry.ruleId === ruleId);
  assert.ok(rule, `read_project reports rule ${ruleId}`);
  return rule.do;
}

/** Rehearse `workspace` with button A pressed on think 2 and released on think 3. */
function rehearse(workspace: AuthoringWorkspace): Promise<SimulationRun> {
  return workspace.adapter.run({
    brainDef: workspace.brainDef,
    scenario: {
      seed: 20260905,
      subject: SUBJECT,
      inputs: [
        { kind: "button-a", at: 2, value: true },
        { kind: "button-a", at: 3, value: false },
      ],
    },
    thinks: 8,
  });
}

describe("the catalog line a drawn image serves the model", () => {
  test("lists it in the document's own scope under the name the user gave it", () => {
    const { workspace, literal } = drawnImageSession();

    const listed = catalogTilesInScope(readCatalog(workspace, {}), CatalogScope.Document);

    const drawn = listed.find((tile: CatalogTile) => tile.tileId === literal.tileId);
    assert.ok(drawn, `the document scope lists ${literal.tileId}`);
    assert.equal(drawn.label, kImageName);
    assert.equal(drawn.kind, "literal");
  });

  test("finds it by a filter on that name", () => {
    const { workspace, literal } = drawnImageSession();

    const found = catalogTiles(readCatalog(workspace, { filter: kImageName }));

    assert.ok(
      found.some((tile) => tile.tileId === literal.tileId),
      found.map((tile) => tile.tileId).join(", ")
    );
  });
});

describe("the project view a drawn image appears in", () => {
  test("names the placed image by its own word", () => {
    const { workspace, literal, ruleId } = drawnImageSession();

    const placed = projectDoTiles(workspace, ruleId);

    assert.deepEqual(
      placed.map((tile) => tile.tileId),
      [mkActuatorTileId(MicroBitV2HostActions.DrawImage.key), literal.tileId]
    );
    assert.deepEqual(
      placed.filter((tile) => tile.tileId === literal.tileId),
      [{ tileId: literal.tileId, label: kImageName }]
    );
  });

  test("keeps the tile id and the placement across an edit of the pixels", () => {
    const { workspace, literal, ruleId } = drawnImageSession();
    const before = projectDoTiles(workspace, ruleId);
    const documentTilesBefore = workspace.brainDef.catalog().getAll().size();

    workspace.history.executeCommand(
      new EditLiteralCommand(workspace.brainDef, literal, { value: imageValue(kEditedDigits) })
    );

    assert.deepEqual(projectDoTiles(workspace, ruleId), before);
    const edited = workspace.brainDef.catalog().get(literal.tileId) as BrainTileLiteralDef | undefined;
    assert.ok(edited, "the catalog still holds the literal under its own id");
    assert.deepEqual(imageGridBytes(edited.value), expectedBytes(kEditedDigits));
    assert.equal(
      workspace.brainDef.catalog().getAll().size(),
      documentTilesBefore,
      "an edit in place mints no second catalog entry"
    );
  });

  test("reports the edited image under the same label the catalog reads it by", () => {
    const { workspace, literal, ruleId } = drawnImageSession();

    workspace.history.executeCommand(
      new EditLiteralCommand(workspace.brainDef, literal, { value: imageValue(kEditedDigits) })
    );

    const placed = projectDoTiles(workspace, ruleId).find((tile) => tile.tileId === literal.tileId);
    const listed = catalogTilesInScope(readCatalog(workspace, {}), CatalogScope.Document).find(
      (tile) => tile.tileId === literal.tileId
    );
    assert.equal(placed?.label, kImageName);
    assert.equal(listed?.label, kImageName);
  });
});

describe("the trace a rehearsal records of a drawn image", () => {
  test("names the image in the draw's arguments rather than spelling out its contents", async () => {
    const { workspace, literal } = drawnImageSession();

    const run = await rehearse(workspace);

    const draws = run.observations.flatMap((think) =>
      think.dispatches.filter((dispatch) => dispatch.action === MicroBitV2HostActions.DrawImage.key)
    );
    assert.equal(draws.length, 1);
    const args = draws[0]?.args ?? [];
    assert.equal(args.length, 1);
    const drawn = args[0] ?? "";
    assert.ok(drawn.endsWith(`[${kImageName}]`), drawn);
    assert.ok(!drawn.includes("buffer#"), `the image's contents are named, not spelled out: ${drawn}`);
    assert.ok(literal.uniqueId !== undefined && !drawn.includes(literal.uniqueId), drawn);
  });
});
