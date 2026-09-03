import {
  bag,
  CoreParameterId,
  type CreateHostActuatorOptions,
  choice,
  type ExecutionContext,
  getSlotId,
  getWhenResult,
  isBooleanValue,
  isNilValue,
  isNumberValue,
  isStringValue,
  mkCallDef,
  optional,
  param,
  type ReadonlyList,
  type Value,
  VOID_VALUE,
} from "@wendoo/core/app";
import { type BufferValue, bufferByteAt, bufferLength, isBufferValue } from "@wendoo/core/runtime";
import { RadioPacketType, type RadioSendRecord, radioNumberIsInteger } from "../../../../core/radio";
import { getMicroBitContextDevice } from "../context";
import { MicroBitV2HostActions, WodalMicroBitV2ParameterId } from "../tile-ids";

const EMPTY_BYTES = new Uint8Array(0);

/**
 * What an empty payload slot means: the value the rule's WHEN side found. Each
 * alternative names itself, since one name identifies one spec to the grammar.
 */
const message = { anonymous: true, derived: true } as const;

const AnonNumber = param(CoreParameterId.AnonymousNumber, { ...message, name: "message-number" });
const AnonString = param(CoreParameterId.AnonymousString, { ...message, name: "message-text" });
const AnonBoolean = param(CoreParameterId.AnonymousBoolean, { ...message, name: "message-yes-no" });
const AnonBuffer = param(WodalMicroBitV2ParameterId.Buffer, { ...message, name: "message-bytes" });

const callDef = mkCallDef(bag(optional(choice(AnonNumber, AnonString, AnonBoolean, AnonBuffer))));

const kNumberSlotId = getSlotId(callDef, AnonNumber);
const kStringSlotId = getSlotId(callDef, AnonString);
const kBooleanSlotId = getSlotId(callDef, AnonBoolean);
const kBufferSlotId = getSlotId(callDef, AnonBuffer);

function presentArg(args: ReadonlyList<Value>, slotId: number): Value | undefined {
  const value = args.at(slotId);
  return value !== undefined && !isNilValue(value) ? value : undefined;
}

/** The explicit anonymous argument if present, otherwise the rule's captured WHEN-result. */
function valueToSend(ctx: ExecutionContext, args: ReadonlyList<Value>): Value {
  return (
    presentArg(args, kNumberSlotId) ??
    presentArg(args, kStringSlotId) ??
    presentArg(args, kBooleanSlotId) ??
    presentArg(args, kBufferSlotId) ??
    getWhenResult(ctx)
  );
}

/** Copies a Buffer value's bytes into a plain byte array. */
function bufferBytes(value: BufferValue): Uint8Array {
  const length = bufferLength(value);
  const bytes = new Uint8Array(length);
  for (let i = 0; i < length; i++) {
    bytes[i] = bufferByteAt(value, i) ?? 0;
  }
  return bytes;
}

/**
 * Builds the typed packet for a sendable value, or undefined when the value is
 * not a String / Number / Boolean / Buffer. A Number sends as NUMBER (integer)
 * or DOUBLE (non-integer); a Boolean sends as a NUMBER 0 / 1; a Buffer sends as
 * a BUFFER packet.
 */
function recordFor(value: Value, group: number): RadioSendRecord | undefined {
  if (isNumberValue(value)) {
    const n = value.v;
    return {
      type: radioNumberIsInteger(n) ? RadioPacketType.Number : RadioPacketType.Double,
      group,
      value: n,
      name: "",
      text: "",
      bytes: EMPTY_BYTES,
    };
  }
  if (isStringValue(value)) {
    return { type: RadioPacketType.String, group, value: 0, name: "", text: value.v, bytes: EMPTY_BYTES };
  }
  if (isBooleanValue(value)) {
    return { type: RadioPacketType.Number, group, value: value.v ? 1 : 0, name: "", text: "", bytes: EMPTY_BYTES };
  }
  if (isBufferValue(value)) {
    return { type: RadioPacketType.Buffer, group, value: 0, name: "", text: "", bytes: bufferBytes(value) };
  }
  return undefined;
}

function execRadioSend(ctx: ExecutionContext, args: ReadonlyList<Value>): Value {
  const microbit = getMicroBitContextDevice(ctx);
  if (!microbit) {
    return VOID_VALUE;
  }
  const record = recordFor(valueToSend(ctx, args), microbit.radio.group);
  if (record !== undefined) {
    microbit.radio.send(record);
  }
  return VOID_VALUE;
}

/**
 * Host actuator: broadcast a radio packet. With an explicit value (String,
 * Number, Boolean, or Buffer) it sends that; with no argument it falls back to
 * the rule's WHEN-result. A non-sendable value is a silent no-op. The group is
 * the device's current radio group, not an argument.
 */
export default {
  ...MicroBitV2HostActions.RadioSend,
  callDef,
  fn: { exec: execRadioSend },
  isAsync: false,
  metadata: { label: "radio send" },
} satisfies CreateHostActuatorOptions;
