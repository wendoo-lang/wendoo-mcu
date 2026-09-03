import assert from "node:assert/strict";
import { describe, test } from "node:test";
import { catalogTiles, createAuthoringWorkspace, readCatalog } from "@wendoo/assistant-bridge";
import { FAKE_TARGET_IDENTITY } from "@wendoo/assistant-bridge/testing";
import { DEFAULT_BUILT_IN_SOUND_NAME } from "../wendoo/built-in-sounds";
import { createTargetAdapter } from "./adapter";

/** Tile id the play-sound actuator is addressed by. */
const PLAY_SOUND_TILE_ID = "tile.actuator->microbit-v2.play-sound";

/** Tile id the play-tone actuator is addressed by. */
const PLAY_TONE_TILE_ID = "tile.actuator->microbit-v2.play-tone";

/** The argument grammar the catalog reports for `tileId`, which must be present. */
function argsOf(tileId: string): string {
  const workspace = createAuthoringWorkspace(createTargetAdapter(FAKE_TARGET_IDENTITY), "declared defaults");
  const tile = catalogTiles(readCatalog(workspace, {})).find((entry) => entry.tileId === tileId);
  assert.ok(tile, tileId);
  assert.ok(tile.args, `${tileId} reports an argument grammar`);
  return tile.args;
}

describe("the declared defaults the catalog reads out", () => {
  test("reads a struct default this target registered out by the word its literal reads by", () => {
    const args = argsOf(PLAY_SOUND_TILE_ID);

    assert.ok(args.includes(`=${DEFAULT_BUILT_IN_SOUND_NAME}`), args);
  });

  test("names no struct type id where a struct default stands", () => {
    const args = argsOf(PLAY_SOUND_TILE_ID);

    assert.ok(!args.includes("=struct:"), args);
  });

  test("reads a numeric default out as the number the implementation holds", () => {
    const args = argsOf(PLAY_TONE_TILE_ID);

    assert.ok(args.includes("(Hz)=880"), args);
  });
});
