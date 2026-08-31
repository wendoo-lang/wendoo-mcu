import {
  type AsyncHandle,
  bag,
  CoreParameterId,
  type CreateHostActuatorOptions,
  choice,
  type ExecutionContext,
  extractNumberValue,
  getSlotId,
  mkCallDef,
  optional,
  param,
  type ReadonlyList,
  type Value,
  VOID_VALUE,
} from "@wendoo/core/app";
import { mkSpeakerToneCommand, type SpeakerToneWaveform } from "../../microbit-speaker";
import { getMicroBitContextDevice, reportDeviceOperationEnding } from "../context";
import { hasModifier, Modifier } from "../modifiers";
import { Param } from "../parameters";
import { MicroBitV2HostActions } from "../tile-ids";

/** Pitch in Hz a tone sounds at when the call omits its frequency. */
export const DEFAULT_FREQUENCY_HZ = 880;

/** Seconds a tone sounds for when the call omits its duration. */
export const DEFAULT_DURATION_SECONDS = 0.5;

/** Fraction of full tone volume a tone sounds at when the call omits its volume. */
export const DEFAULT_VOLUME = 1;

/** Wave shape a tone sounds with when the call names none. */
export const DEFAULT_WAVEFORM: SpeakerToneWaveform = "triangle";

const AnonFrequency = param(CoreParameterId.AnonymousNumber, { anonymous: true });

const callDef = mkCallDef(
  bag(
    optional(AnonFrequency),
    optional(Param.duration),
    optional(Param.volume),
    optional(choice(Modifier.square, Modifier.sawtooth, Modifier.sine, Modifier.triangle)),
    optional(Modifier.immediately),
    optional(Modifier.inBackground)
  )
);

const kFrequencySlotId = getSlotId(callDef, AnonFrequency);
const kDurationSlotId = getSlotId(callDef, Param.duration);
const kVolumeSlotId = getSlotId(callDef, Param.volume);
const kSquareSlotId = getSlotId(callDef, Modifier.square);
const kSawtoothSlotId = getSlotId(callDef, Modifier.sawtooth);
const kSineSlotId = getSlotId(callDef, Modifier.sine);
const kTriangleSlotId = getSlotId(callDef, Modifier.triangle);
const kImmediatelySlotId = getSlotId(callDef, Modifier.immediately);
const kInBackgroundSlotId = getSlotId(callDef, Modifier.inBackground);

/** The number in `slotId`, or `fallback` when the slot is absent, nil, or non-finite. */
function finiteArg(args: ReadonlyList<Value>, slotId: number, fallback: number): number {
  const value = extractNumberValue(args.at(slotId));
  return value === undefined || !Number.isFinite(value) ? fallback : value;
}

/** The wave shape the attached modifier selects; triangle when none is attached. */
function selectedWaveform(args: ReadonlyList<Value>): SpeakerToneWaveform {
  if (hasModifier(args, kSawtoothSlotId)) {
    return "sawtooth";
  }
  if (hasModifier(args, kSineSlotId)) {
    return "sine";
  }
  if (hasModifier(args, kTriangleSlotId)) {
    return "triangle";
  }
  if (hasModifier(args, kSquareSlotId)) {
    return "square";
  }
  return DEFAULT_WAVEFORM;
}

function execPlayTone(ctx: ExecutionContext, args: ReadonlyList<Value>, handle: AsyncHandle): void {
  const microbit = getMicroBitContextDevice(ctx);
  if (!microbit) {
    handle.resolve(VOID_VALUE);
    return;
  }
  if (hasModifier(args, kImmediatelySlotId)) {
    microbit.speaker.preempt();
  }
  const durationSeconds = finiteArg(args, kDurationSlotId, DEFAULT_DURATION_SECONDS);
  // Convert the seconds argument to whole ms at f32 precision, matching the device.
  const command = mkSpeakerToneCommand(
    selectedWaveform(args),
    finiteArg(args, kFrequencySlotId, DEFAULT_FREQUENCY_HZ),
    Math.round(Math.fround(durationSeconds * 1000)),
    finiteArg(args, kVolumeSlotId, DEFAULT_VOLUME)
  );
  const inBackground = hasModifier(args, kInBackgroundSlotId);
  microbit.speaker.playTone(command, ctx.time, (end) => {
    handle.resolve(VOID_VALUE);
    reportDeviceOperationEnding(ctx, { handleId: handle.id, end, inBackground });
  });
  if (inBackground) {
    // The tone keeps its speaker lease and resolves on tick time as above;
    // resolving now releases the issuing rule so it does not park on the tone.
    handle.resolve(VOID_VALUE);
  }
}

/**
 * Host actuator: sound a plain constant-pitch tone on the speaker. Args: the
 * optional anonymous pitch in Hz (default 880, clamped into 0-9999, where 0 is
 * a silent rest that still holds the speaker), the named duration in seconds
 * (default 0.5), the named volume as a 0-1 fraction of full (default 1,
 * clamped), and at most one wave-shape modifier (`square`, `sawtooth`, `sine`,
 * or `triangle`; triangle when none is attached). An absent, nil, or non-finite
 * number reads as its default. Asynchronous -- the calling fiber awaits the
 * returned handle and resumes when the duration elapses. A negative duration
 * sounds nothing and resolves at once. With the `immediately` modifier the
 * current speaker lease is preempted at dispatch so the tone starts at once;
 * otherwise a tone requested while the speaker is busy is dropped. With the
 * `in background` modifier the tone keeps its lease but the handle resolves at
 * dispatch, so the issuing rule continues this round without parking on it.
 */
export default {
  ...MicroBitV2HostActions.PlayTone,
  callDef,
  fn: { exec: execPlayTone },
  isAsync: true,
  metadata: {
    label: "beep",
    grammarNote:
      'Plays a plain tone: the bare number is the pitch in Hz (default 880; 0 plays silence for the duration, a rest), "duration" is in seconds (default 0.5), "volume" is 0 to 1 (default 1), and one wave-shape word (square / sawtooth / sine / triangle) picks the sound, triangle when none is given. The rule holds until the tone ends: until then a rule under it does not get its turn, and this rule cannot fire again. A tone asked for while a sound is playing is dropped; add "in background" to let the rule carry on, or "immediately" to cut off the sound that is playing.',
  },
} satisfies CreateHostActuatorOptions;
