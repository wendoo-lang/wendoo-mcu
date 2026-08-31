import { isNilValue, type ModifierTileInput, mod, type ReadonlyList, type Value } from "@wendoo/core/app";
import { WodalMicroBitV2ModifierId } from "./tile-ids";

/** Whether the modifier arg at `slotId` is present (a non-nil value), i.e. its tile is attached. */
export function hasModifier(args: ReadonlyList<Value>, slotId: number): boolean {
  const value = args.at(slotId);
  return value !== undefined && !isNilValue(value);
}

/**
 * Shared modifier call-spec arg specs, defined once and reused across the
 * sensor and actuator tiles of this module.
 */
export const Modifier = {
  pressed: mod(WodalMicroBitV2ModifierId.Pressed),
  released: mod(WodalMicroBitV2ModifierId.Released),
  click: mod(WodalMicroBitV2ModifierId.Click),
  doubleClick: mod(WodalMicroBitV2ModifierId.DoubleClick),
  longClick: mod(WodalMicroBitV2ModifierId.LongClick),
  held: mod(WodalMicroBitV2ModifierId.Held),
  shake: mod(WodalMicroBitV2ModifierId.Shake),
  tiltUp: mod(WodalMicroBitV2ModifierId.TiltUp),
  tiltDown: mod(WodalMicroBitV2ModifierId.TiltDown),
  tiltLeft: mod(WodalMicroBitV2ModifierId.TiltLeft),
  tiltRight: mod(WodalMicroBitV2ModifierId.TiltRight),
  faceUp: mod(WodalMicroBitV2ModifierId.FaceUp),
  faceDown: mod(WodalMicroBitV2ModifierId.FaceDown),
  freefall: mod(WodalMicroBitV2ModifierId.Freefall),
  immediately: mod(WodalMicroBitV2ModifierId.Immediately),
  inBackground: mod(WodalMicroBitV2ModifierId.InBackground),
  square: mod(WodalMicroBitV2ModifierId.Square),
  sawtooth: mod(WodalMicroBitV2ModifierId.Sawtooth),
  sine: mod(WodalMicroBitV2ModifierId.Sine),
  triangle: mod(WodalMicroBitV2ModifierId.Triangle),
};

/** Modifier tiles registered once with the module. */
export const MICROBIT_V2_MODIFIERS: readonly ModifierTileInput[] = [
  { id: WodalMicroBitV2ModifierId.Pressed, label: "pressed" },
  { id: WodalMicroBitV2ModifierId.Released, label: "released" },
  { id: WodalMicroBitV2ModifierId.Click, label: "click" },
  { id: WodalMicroBitV2ModifierId.DoubleClick, label: "double click" },
  { id: WodalMicroBitV2ModifierId.LongClick, label: "long click" },
  { id: WodalMicroBitV2ModifierId.Held, label: "held" },
  { id: WodalMicroBitV2ModifierId.Shake, label: "shake" },
  { id: WodalMicroBitV2ModifierId.TiltUp, label: "tilt up" },
  { id: WodalMicroBitV2ModifierId.TiltDown, label: "tilt down" },
  { id: WodalMicroBitV2ModifierId.TiltLeft, label: "tilt left" },
  { id: WodalMicroBitV2ModifierId.TiltRight, label: "tilt right" },
  { id: WodalMicroBitV2ModifierId.FaceUp, label: "face up" },
  { id: WodalMicroBitV2ModifierId.FaceDown, label: "face down" },
  { id: WodalMicroBitV2ModifierId.Freefall, label: "freefall" },
  { id: WodalMicroBitV2ModifierId.Immediately, label: "immediately" },
  { id: WodalMicroBitV2ModifierId.InBackground, label: "in background" },
  { id: WodalMicroBitV2ModifierId.Square, label: "square" },
  { id: WodalMicroBitV2ModifierId.Sawtooth, label: "sawtooth" },
  { id: WodalMicroBitV2ModifierId.Sine, label: "sine" },
  { id: WodalMicroBitV2ModifierId.Triangle, label: "triangle" },
];
