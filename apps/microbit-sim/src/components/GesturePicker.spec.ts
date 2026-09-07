import assert from "node:assert/strict";
import { describe, test } from "node:test";
import { AccelerometerGesture, GestureInjector, MicroBit } from "@wendoo/wodal/targets/microbit-v2";
import { createElement } from "react";
import { renderToStaticMarkup } from "react-dom/server";
import type { SimulatorInstance } from "../services/simulator";
import { GesturePicker } from "./GesturePicker";

/** Wraps a bare `MicroBit` plus a gesture injector as the only instance shape the picker reads. */
function instanceWith(microbit: MicroBit, gestureInjector: GestureInjector): SimulatorInstance {
  return { microbit, gestureInjector } as unknown as SimulatorInstance;
}

/** Renders the picker and returns the numeric gesture code of the selected option. */
function renderedSelectValue(instance: SimulatorInstance): string {
  const markup = renderToStaticMarkup(createElement(GesturePicker, { instance, onSelected: () => {} }));
  assert.match(markup, /data-testid="gesture-select"/, "expected a gesture select");
  const match = markup.match(/<option value="(-?\d+)" selected/);
  assert.ok(match, "expected exactly one selected gesture option");
  return match[1];
}

describe("GesturePicker", () => {
  test("a fresh injector renders the resting none selection", () => {
    const microbit = new MicroBit();
    const instance = instanceWith(microbit, new GestureInjector(microbit.accelerometer));
    assert.equal(renderedSelectValue(instance), String(AccelerometerGesture.None));
  });

  test("a remount recovers the held posture from the injector", () => {
    const microbit = new MicroBit();
    const injector = new GestureInjector(microbit.accelerometer);
    injector.select(AccelerometerGesture.TiltLeft);

    // A fresh render stands in for reopening the Popover: the picker seeds from the injector.
    assert.equal(renderedSelectValue(instanceWith(microbit, injector)), String(AccelerometerGesture.TiltLeft));
  });
});
