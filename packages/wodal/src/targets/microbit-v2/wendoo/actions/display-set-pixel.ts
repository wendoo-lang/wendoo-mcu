import {
  bag,
  type CreateHostActuatorOptions,
  type ExecutionContext,
  extractNumberValue,
  getSlotId,
  mkCallDef,
  mkNumberValue,
  optional,
  type ReadonlyList,
  type Value,
  VOID_VALUE,
} from "@wendoo/core/app";
import { getMicroBitContextDevice } from "../context";
import { MAX_BRIGHTNESS, Param } from "../parameters";
import { MicroBitV2HostActions } from "../tile-ids";
import { brightnessToPort, pixelCoordToPort } from "./display-pixel-conversion";

/** Column a set-pixel writes when the call names no x. */
const DEFAULT_X = 0;

/** Row a set-pixel writes when the call names no y. */
const DEFAULT_Y = 0;

/** LED level a set-pixel writes when the call names no brightness. */
const DEFAULT_BRIGHTNESS = MAX_BRIGHTNESS;

/** The column written; one off the display is not written at all. */
const X = { ...Param.x, default: mkNumberValue(DEFAULT_X) };

/** The row written; one off the display is not written at all. */
const Y = { ...Param.y, default: mkNumberValue(DEFAULT_Y) };

/** The LED level written. */
const Brightness = { ...Param.brightness, default: mkNumberValue(DEFAULT_BRIGHTNESS) };

const callDef = mkCallDef(bag(optional(X), optional(Y), optional(Brightness)));

const kXSlotId = getSlotId(callDef, X);
const kYSlotId = getSlotId(callDef, Y);
const kBrightnessSlotId = getSlotId(callDef, Brightness);

function execDisplaySetPixel(ctx: ExecutionContext, args: ReadonlyList<Value>): Value {
  const microbit = getMicroBitContextDevice(ctx);
  if (!microbit) {
    return VOID_VALUE;
  }
  const x = extractNumberValue(args.at(kXSlotId)) ?? DEFAULT_X;
  const y = extractNumberValue(args.at(kYSlotId)) ?? DEFAULT_Y;
  const brightness = extractNumberValue(args.at(kBrightnessSlotId)) ?? DEFAULT_BRIGHTNESS;
  microbit.display.setPixelValue(pixelCoordToPort(x), pixelCoordToPort(y), brightnessToPort(brightness));
  return VOID_VALUE;
}

/** Host actuator: set one LED pixel brightness on the simulated display. */
export default {
  ...MicroBitV2HostActions.DisplaySetPixel,
  callDef,
  fn: { exec: execDisplaySetPixel },
  isAsync: false,
  metadata: { label: "set pixel" },
} satisfies CreateHostActuatorOptions;
