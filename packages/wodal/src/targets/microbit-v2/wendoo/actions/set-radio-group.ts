import {
  bag,
  CoreParameterId,
  type CreateHostActuatorOptions,
  type ExecutionContext,
  extractNumberValue,
  getSlotId,
  mkCallDef,
  mkNumberValue,
  optional,
  param,
  type ReadonlyList,
  type Value,
  VOID_VALUE,
} from "@wendoo/core/app";
import { RADIO_DEFAULT_GROUP } from "../../../../core/radio";
import { getMicroBitContextDevice } from "../context";
import { MicroBitV2HostActions } from "../tile-ids";

/** Highest radio group; a group past it wraps by modulus. */
const RADIO_MAX_GROUP = 255;

const AnonNumber = param(CoreParameterId.AnonymousNumber, {
  anonymous: true,
  name: "group",
  default: mkNumberValue(RADIO_DEFAULT_GROUP),
  range: { min: 0, max: RADIO_MAX_GROUP, onExceed: "wrap" },
});

const callDef = mkCallDef(bag(optional(AnonNumber)));

const kGroupSlotId = getSlotId(callDef, AnonNumber);

function execSetRadioGroup(ctx: ExecutionContext, args: ReadonlyList<Value>): Value {
  const microbit = getMicroBitContextDevice(ctx);
  if (!microbit) {
    return VOID_VALUE;
  }
  microbit.radio.setGroup(extractNumberValue(args.at(kGroupSlotId)) ?? RADIO_DEFAULT_GROUP);
  return VOID_VALUE;
}

/** Host actuator: set the radio group (0-255). Both ends of a link must agree on the group. */
export default {
  ...MicroBitV2HostActions.SetRadioGroup,
  callDef,
  fn: { exec: execSetRadioGroup },
  isAsync: false,
  metadata: { label: "set radio group" },
} satisfies CreateHostActuatorOptions;
