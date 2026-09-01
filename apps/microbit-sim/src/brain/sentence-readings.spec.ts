/**
 * Pins the sentence every shipped microbit-v2 tile reads in: one arrangement per
 * sensor and actuator of the catalog -- the tile alone, the tile with its first
 * modifier or parameter, and the tile with an object where it takes one -- plus
 * the page paragraph a multi-rule page reads as and the line a rule shows while
 * it is being composed. A wording change moves one of these strings, so a change
 * to tile language metadata or to the projection shows up here as an edit.
 */

import assert from "node:assert/strict";
import { before, describe, test } from "node:test";
import type { BrainServices, IBrainPageDef, IBrainTileDef } from "@wendoo/core/brain";
import {
  mkActuatorTileId,
  mkModifierTileId,
  mkOperatorTileId,
  mkParameterTileId,
  mkSensorTileId,
  RuleTriggerMode,
} from "@wendoo/core/brain";
import { __test__appendTile } from "@wendoo/core/brain/__test__";
import {
  paragraphText,
  projectPageParagraph,
  projectRuleSentence,
  segmentDisplayText,
  sentenceText,
  triggerModeWord,
} from "@wendoo/core/brain/language-service";
import { BrainDef, type BrainPageDef, type BrainRuleDef } from "@wendoo/core/brain/model";
import { BrainTileLiteralDef } from "@wendoo/core/brain/tiles";
import type { Localizer } from "@wendoo/core/localization";
import { createDefaultLocalizer } from "@wendoo/core/localization";
import { CoreHostActions, CoreOpId, CoreTypeIds } from "@wendoo/core/runtime";
import { composePivotReading, composeSentenceReading } from "@wendoo/ui/brain-editor/sentence-composer";
import {
  createMicroBitV2Environment,
  MicroBitV2HostActions,
  WodalMicroBitV2ModifierId as Mod,
  WodalMicroBitV2ParameterId as Param,
} from "@wendoo/wodal/targets/microbit-v2";

let services: BrainServices;
let localizer: Localizer;

before(() => {
  services = createMicroBitV2Environment().brainServices;
  localizer = createDefaultLocalizer();
});

/** The registered catalog tile `tileId` names. */
function tile(tileId: string): IBrainTileDef {
  const tileDef = services.edit.tiles.get(tileId);
  assert.ok(tileDef, `catalog tile ${tileId} is registered`);
  return tileDef;
}

const sensor = (key: string) => tile(mkSensorTileId(key));
const actuator = (key: string) => tile(mkActuatorTileId(key));
const modifier = (id: string) => tile(mkModifierTileId(id));
const parameter = (id: string) => tile(mkParameterTileId(id));
const operator = (opId: string) => tile(mkOperatorTileId(opId));

/** The catalog's literal tile labelled `label`. */
function literal(label: string): IBrainTileDef {
  const all = services.edit.tiles.getAll();
  for (let i = 0; i < all.size(); i++) {
    const tileDef = all.get(i);
    if (tileDef.kind === "literal" && tileDef.metadata?.label === label) {
      return tileDef;
    }
  }
  throw new Error(`no literal tile labelled ${label}`);
}

function number(value: number): IBrainTileDef {
  return new BrainTileLiteralDef(CoreTypeIds.Number, value, {}, services);
}

function text(value: string): IBrainTileDef {
  return new BrainTileLiteralDef(CoreTypeIds.String, value, {}, services);
}

/** A rule of a real brain document carrying the tiles of each side. */
function rule(whenTiles: readonly IBrainTileDef[], doTiles: readonly IBrainTileDef[]): BrainRuleDef {
  const brainDef = BrainDef.emptyBrainDef(services, "microbit-sentence-readings");
  const ruleDef = brainDef.pages().get(0).children().get(0) as BrainRuleDef;
  for (const tileDef of whenTiles) {
    __test__appendTile(ruleDef.when(), tileDef);
  }
  for (const tileDef of doTiles) {
    __test__appendTile(ruleDef.do(), tileDef);
  }
  return ruleDef;
}

/** The settled sentence of a rule holding the tiles of each side. */
function reading(whenTiles: readonly IBrainTileDef[], doTiles: readonly IBrainTileDef[] = []): string {
  return sentenceText(projectRuleSentence(rule(whenTiles, doTiles), localizer), localizer);
}

/** One rule of a page fixture: the tiles of each side, its mode, plus any child rules. */
interface RuleSpec {
  readonly when?: readonly IBrainTileDef[];
  readonly do?: readonly IBrainTileDef[];
  /** The rule's trigger mode; omitted leaves it in the `when` mode. */
  readonly trigger?: RuleTriggerMode;
  readonly children?: readonly RuleSpec[];
}

function appendRuleSpecs(page: BrainPageDef, specs: readonly RuleSpec[], depth: number): void {
  for (const spec of specs) {
    const ruleDef = page.appendNewRule();
    for (const tileDef of spec.when ?? []) {
      __test__appendTile(ruleDef.when(), tileDef);
    }
    for (const tileDef of spec.do ?? []) {
      __test__appendTile(ruleDef.do(), tileDef);
    }
    for (let i = 0; i < depth; i++) {
      assert.ok(ruleDef.indent(), `rule indents to depth ${depth}`);
    }
    if (spec.trigger !== undefined) {
      ruleDef.setTrigger(spec.trigger);
    }
    appendRuleSpecs(page, spec.children ?? [], depth + 1);
  }
}

/** A page of a real brain document holding the rule tree `specs` describes. */
function page(specs: readonly RuleSpec[]): IBrainPageDef {
  const brainDef = BrainDef.emptyBrainDef(services, "microbit-paragraph-readings");
  const pageDef = brainDef.pages().get(0) as BrainPageDef;
  pageDef.removeRuleAtIndex(0);
  appendRuleSpecs(pageDef, specs, 0);
  return pageDef;
}

/** The paragraph of a page holding the rule tree `specs` describes. */
function paragraph(specs: readonly RuleSpec[]): string {
  return paragraphText(projectPageParagraph(page(specs), localizer), localizer);
}

/** The line a rule shows mid-composition, with the pivot comma when `pivoted`. */
function composing(
  whenTiles: readonly IBrainTileDef[],
  doTiles: readonly IBrainTileDef[] = [],
  pivoted = false
): string {
  const settled = projectRuleSentence(rule(whenTiles, doTiles), localizer).toArray();
  const composed = composeSentenceReading(settled);
  const segments = pivoted
    ? [...composed, ...composePivotReading(composed, triggerModeWord(RuleTriggerMode.When, localizer))]
    : composed;
  return segments.map((segment) => segmentDisplayText(segment, localizer)).join("");
}

// -- sensors ------------------------------------------------------------------

describe("microbit-v2 sensor readings", () => {
  test("a button sensor alone reads the press its bare word names", () => {
    assert.equal(reading([sensor(MicroBitV2HostActions.ButtonA.key)]), "When button A pressed,");
    assert.equal(reading([sensor(MicroBitV2HostActions.ButtonB.key)]), "When button B pressed,");
    assert.equal(reading([sensor(MicroBitV2HostActions.ButtonAB.key)]), "When button A+B pressed,");
    assert.equal(reading([sensor(MicroBitV2HostActions.ButtonLogo.key)]), "When logo pressed,");
  });

  test("a button sensor reads the event its modifier selects", () => {
    const buttonA = () => sensor(MicroBitV2HostActions.ButtonA.key);

    assert.equal(reading([buttonA(), modifier(Mod.Pressed)]), "When button A pressed,");
    assert.equal(reading([buttonA(), modifier(Mod.Released)]), "When button A released,");
    assert.equal(reading([buttonA(), modifier(Mod.Click)]), "When button A click,");
    assert.equal(reading([buttonA(), modifier(Mod.DoubleClick)]), "When button A double click,");
    assert.equal(reading([buttonA(), modifier(Mod.LongClick)]), "When button A long click,");
    assert.equal(reading([buttonA(), modifier(Mod.Held)]), "When button A held,");
  });

  test("the gesture sensor alone reads the shake its bare word names", () => {
    assert.equal(reading([sensor(MicroBitV2HostActions.Gesture.key)]), "When gesture shake,");
  });

  test("the gesture sensor reads the gesture its modifier selects", () => {
    const gesture = () => sensor(MicroBitV2HostActions.Gesture.key);

    assert.equal(reading([gesture(), modifier(Mod.Shake)]), "When gesture shake,");
    assert.equal(reading([gesture(), modifier(Mod.TiltUp)]), "When gesture tilt up,");
    assert.equal(reading([gesture(), modifier(Mod.TiltDown)]), "When gesture tilt down,");
    assert.equal(reading([gesture(), modifier(Mod.TiltLeft)]), "When gesture tilt left,");
    assert.equal(reading([gesture(), modifier(Mod.TiltRight)]), "When gesture tilt right,");
    assert.equal(reading([gesture(), modifier(Mod.FaceUp)]), "When gesture face up,");
    assert.equal(reading([gesture(), modifier(Mod.FaceDown)]), "When gesture face down,");
    assert.equal(reading([gesture(), modifier(Mod.Freefall)]), "When gesture freefall,");
  });

  test("a value sensor alone reads with no subject", () => {
    assert.equal(reading([sensor(MicroBitV2HostActions.LightLevel.key)]), "When light level,");
    assert.equal(reading([sensor(MicroBitV2HostActions.Temperature.key)]), "When temperature,");
  });

  test("a value sensor inside a comparison reads with no subject", () => {
    assert.equal(
      reading([sensor(MicroBitV2HostActions.LightLevel.key), operator(CoreOpId.GreaterThan), number(200)]),
      "When light level is greater than 200,"
    );
    assert.equal(
      reading([sensor(MicroBitV2HostActions.Temperature.key), operator(CoreOpId.LessThan), number(10)]),
      "When temperature is less than 10,"
    );
  });

  test("a radio-receive sensor alone reads through its event frame", () => {
    assert.equal(reading([sensor(MicroBitV2HostActions.RadioReceiveNumber.key)]), "When radio receive number,");
    assert.equal(reading([sensor(MicroBitV2HostActions.RadioReceiveString.key)]), "When radio receive string,");
    assert.equal(reading([sensor(MicroBitV2HostActions.RadioReceiveBuffer.key)]), "When radio receive buffer,");
  });
});

// -- actuators ----------------------------------------------------------------

describe("microbit-v2 actuator readings", () => {
  test("the display-text actuator reads its words and its timing modifiers", () => {
    const displayText = () => actuator(MicroBitV2HostActions.DisplayScroll.key);

    assert.equal(reading([], [displayText()]), "Always, display text.");
    assert.equal(reading([], [displayText(), text("hi")]), 'Always, display text "hi".');
    assert.equal(
      reading([], [displayText(), text("hi"), modifier(Mod.Immediately)]),
      'Always, display text "hi" immediately.'
    );
    assert.equal(
      reading([], [displayText(), text("hi"), modifier(Mod.InBackground)]),
      'Always, display text "hi" in background.'
    );
  });

  test("the draw-image actuator reads its image and its duration", () => {
    const drawImage = () => actuator(MicroBitV2HostActions.DrawImage.key);

    assert.equal(reading([], [drawImage()]), "Always, draw image.");
    assert.equal(reading([], [drawImage(), literal("heart")]), "Always, draw image heart.");
    assert.equal(
      reading([], [drawImage(), literal("heart"), parameter(Param.Duration), number(2)]),
      "Always, draw image heart duration 2."
    );
  });

  test("the set-pixel actuator reads its coordinates and its brightness", () => {
    const setPixel = () => actuator(MicroBitV2HostActions.DisplaySetPixel.key);

    assert.equal(reading([], [setPixel()]), "Always, set pixel.");
    assert.equal(
      reading([], [setPixel(), parameter(Param.X), number(1), parameter(Param.Y), number(2)]),
      "Always, set pixel x 1 y 2."
    );
    assert.equal(
      reading([], [setPixel(), parameter(Param.Brightness), number(255)]),
      "Always, set pixel brightness 255."
    );
  });

  test("the play-sound actuator reads its sound and its timing modifiers", () => {
    const playSound = () => actuator(MicroBitV2HostActions.PlaySound.key);

    assert.equal(reading([], [playSound()]), "Always, play sound.");
    assert.equal(reading([], [playSound(), literal("giggle")]), "Always, play sound giggle.");
    assert.equal(
      reading([], [playSound(), literal("giggle"), modifier(Mod.InBackground)]),
      "Always, play sound giggle in background."
    );
  });

  test("the display-clear actuator reads as its own word", () => {
    assert.equal(reading([], [actuator(MicroBitV2HostActions.DisplayClear.key)]), "Always, clear display.");
  });

  test("the radio actuators read their value", () => {
    assert.equal(reading([], [actuator(MicroBitV2HostActions.RadioSend.key)]), "Always, radio send.");
    assert.equal(reading([], [actuator(MicroBitV2HostActions.RadioSend.key), number(7)]), "Always, radio send 7.");
    assert.equal(reading([], [actuator(MicroBitV2HostActions.SetRadioGroup.key)]), "Always, set radio group.");
    assert.equal(
      reading([], [actuator(MicroBitV2HostActions.SetRadioGroup.key), number(3)]),
      "Always, set radio group 3."
    );
  });

  test("a sensor output tile placed on the DO side reads as the output's name", () => {
    assert.equal(
      reading(
        [sensor(MicroBitV2HostActions.RadioReceiveNumber.key)],
        [actuator(MicroBitV2HostActions.DisplayScroll.key), tile("tile.out->number:<number>.value")]
      ),
      "When radio receive number, display text received value."
    );
    assert.equal(
      reading(
        [sensor(MicroBitV2HostActions.RadioReceiveNumber.key)],
        [actuator(MicroBitV2HostActions.DisplayScroll.key), tile("tile.out->number:<number>.rssi")]
      ),
      "When radio receive number, display text signal strength."
    );
  });
});

// -- paragraphs ---------------------------------------------------------------

describe("microbit-v2 paragraph readings", () => {
  const buttonA = () => sensor(MicroBitV2HostActions.ButtonA.key);
  const gesture = () => sensor(MicroBitV2HostActions.Gesture.key);
  const displayText = () => actuator(MicroBitV2HostActions.DisplayScroll.key);
  const playSound = () => actuator(MicroBitV2HostActions.PlaySound.key);
  const clear = () => actuator(MicroBitV2HostActions.DisplayClear.key);

  test("a flat page reads as one sentence per rule", () => {
    assert.equal(
      paragraph([
        { when: [buttonA()], do: [displayText(), text("A")] },
        { when: [gesture()], do: [playSound(), literal("giggle")] },
        {
          when: [sensor(MicroBitV2HostActions.LightLevel.key), operator(CoreOpId.GreaterThan), number(200)],
          do: [clear()],
        },
      ]),
      'When button A pressed, display text "A". When gesture shake, play sound giggle. When light level is greater than 200, clear display.'
    );
  });

  test("a parent with one child reads as one sentence with a continuation", () => {
    assert.equal(
      paragraph([
        { when: [buttonA()], do: [displayText(), text("A")], children: [{ when: [gesture()], do: [clear()] }] },
      ]),
      'When button A pressed, display text "A", and if gesture shake, clear display.'
    );
  });

  test("an always-headed parent carries its child's continuation", () => {
    assert.equal(
      paragraph([{ do: [displayText(), text("hi")], children: [{ when: [buttonA()], do: [clear()] }] }]),
      'Always, display text "hi", and if button A pressed, clear display.'
    );
  });

  test("two children of one parent and a chain of two read the same continuations", () => {
    const breadth = paragraph([
      {
        when: [buttonA()],
        do: [displayText(), text("A")],
        children: [
          { when: [gesture()], do: [playSound(), literal("giggle")] },
          {
            when: [sensor(MicroBitV2HostActions.Temperature.key), operator(CoreOpId.GreaterThan), number(30)],
            do: [clear()],
          },
        ],
      },
    ]);
    const depth = paragraph([
      {
        when: [buttonA()],
        do: [displayText(), text("A")],
        children: [
          {
            when: [gesture()],
            do: [playSound(), literal("giggle")],
            children: [
              {
                when: [sensor(MicroBitV2HostActions.Temperature.key), operator(CoreOpId.GreaterThan), number(30)],
                do: [clear()],
              },
            ],
          },
        ],
      },
    ]);

    assert.equal(
      breadth,
      'When button A pressed, display text "A", and if gesture shake, play sound giggle, and if temperature is greater than 30, clear display.'
    );
    assert.equal(depth, breadth);
  });

  test("a parent with no action of its own is completed by its child's clause", () => {
    assert.equal(
      paragraph([{ when: [sensor(CoreHostActions.Otherwise.key)], children: [{ when: [buttonA()], do: [clear()] }] }]),
      "Otherwise, when button A pressed, clear display."
    );
  });

  test("a radio sender and receiver read as two sentences", () => {
    assert.equal(
      paragraph([
        { when: [buttonA()], do: [actuator(MicroBitV2HostActions.RadioSend.key), number(1)] },
        {
          when: [sensor(MicroBitV2HostActions.RadioReceiveNumber.key)],
          do: [displayText(), tile("tile.out->number:<number>.value")],
        },
      ]),
      "When button A pressed, radio send 1. When radio receive number, display text received value."
    );
  });
});

// -- trigger modes ------------------------------------------------------------

describe("microbit-v2 trigger mode readings", () => {
  const buttonA = () => sensor(MicroBitV2HostActions.ButtonA.key);
  const gesture = () => sensor(MicroBitV2HostActions.Gesture.key);
  const displayText = () => actuator(MicroBitV2HostActions.DisplayScroll.key);
  const playSound = () => actuator(MicroBitV2HostActions.PlaySound.key);
  const clear = () => actuator(MicroBitV2HostActions.DisplayClear.key);

  test("an otherwise rule reads its connective, alone and before a condition", () => {
    assert.equal(
      paragraph([
        { when: [buttonA()], do: [displayText(), text("hi")] },
        { do: [clear()], trigger: RuleTriggerMode.Otherwise },
      ]),
      'When button A pressed, display text "hi". Otherwise, clear display.'
    );
    assert.equal(
      paragraph([
        { when: [buttonA()], do: [displayText(), text("hi")] },
        { when: [gesture()], do: [clear()], trigger: RuleTriggerMode.Otherwise },
      ]),
      'When button A pressed, display text "hi". Otherwise, when gesture shake, clear display.'
    );
  });

  test("a then rule reads its connective, alone and before a condition", () => {
    assert.equal(
      paragraph([
        { when: [buttonA()], do: [displayText(), text("hi")] },
        { do: [playSound(), literal("giggle")], trigger: RuleTriggerMode.Then },
      ]),
      'When button A pressed, display text "hi". Then, play sound giggle.'
    );
    assert.equal(
      paragraph([
        { when: [buttonA()], do: [displayText(), text("hi")] },
        { when: [gesture()], do: [displayText(), text("bye")], trigger: RuleTriggerMode.Then },
      ]),
      'When button A pressed, display text "hi". Then, when gesture shake, display text "bye".'
    );
  });

  test("a mode child continues its parent's sentence through its own connective", () => {
    assert.equal(
      paragraph([
        {
          when: [buttonA()],
          do: [displayText(), text("hi")],
          children: [
            { do: [playSound(), literal("giggle")], trigger: RuleTriggerMode.Then },
            { do: [clear()], trigger: RuleTriggerMode.Otherwise },
          ],
        },
      ]),
      'When button A pressed, display text "hi", then play sound giggle, otherwise clear display.'
    );
  });
});

// -- composition --------------------------------------------------------------

describe("microbit-v2 composition readings", () => {
  test("a bare word the frame supplies stays out of the composed line", () => {
    assert.equal(composing([sensor(MicroBitV2HostActions.ButtonA.key)]), "When button A");
    assert.equal(composing([sensor(MicroBitV2HostActions.Gesture.key)]), "When gesture");
  });

  test("a partial WHEN side reads only the words placed on it", () => {
    assert.equal(
      composing([sensor(MicroBitV2HostActions.LightLevel.key), operator(CoreOpId.GreaterThan)]),
      "When light level is greater than"
    );
  });

  test("the pivot reads the comma, and the trigger word before it on an empty WHEN side", () => {
    assert.equal(composing([sensor(MicroBitV2HostActions.ButtonA.key)], [], true), "When button A,");
    assert.equal(composing([], [], true), "Always,");
  });

  test("a rule composed on both sides reads as one clause with no terminator", () => {
    assert.equal(
      composing([sensor(MicroBitV2HostActions.ButtonA.key)], [actuator(MicroBitV2HostActions.DisplayClear.key)]),
      "When button A, clear display"
    );
  });
});
