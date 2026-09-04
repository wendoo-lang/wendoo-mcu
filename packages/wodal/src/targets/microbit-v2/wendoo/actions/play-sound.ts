import {
  type AsyncHandle,
  bag,
  type CreateHostActuatorOptions,
  type ExecutionContext,
  extractStringValue,
  getSlotId,
  isStructValue,
  mkCallDef,
  optional,
  type ReadonlyList,
  type Value,
  VOID_VALUE,
} from "@wendoo/core/app";
import {
  builtInSoundStructValue,
  DEFAULT_BUILT_IN_SOUND_NAME,
  findBuiltInSound,
  SoundEmojiField,
} from "../built-in-sounds";
import { getMicroBitContextDevice, reportDeviceOperationEnding } from "../context";
import { hasModifier, Modifier } from "../modifiers";
import { Param } from "../parameters";
import { MicroBitV2HostActions } from "../tile-ids";

/** The built-in sound a call naming none plays. */
const Sound = { ...Param.soundEmoji, default: builtInSoundStructValue(findBuiltInSound(DEFAULT_BUILT_IN_SOUND_NAME)!) };

const callDef = mkCallDef(bag(optional(Sound), optional(Modifier.immediately), optional(Modifier.inBackground)));

const kSoundSlotId = getSlotId(callDef, Sound);
const kImmediatelySlotId = getSlotId(callDef, Modifier.immediately);
const kInBackgroundSlotId = getSlotId(callDef, Modifier.inBackground);

/**
 * The sound name carried by a `SoundEmoji` struct value, or undefined when the
 * value is not one (a struct with a string `name` field).
 */
function soundEmojiName(value: Value | undefined): string | undefined {
  if (value === undefined || !isStructValue(value) || value.v === undefined) {
    return undefined;
  }
  return extractStringValue(value.v.at(SoundEmojiField.Name));
}

function execPlaySound(ctx: ExecutionContext, args: ReadonlyList<Value>, handle: AsyncHandle): void {
  const name = soundEmojiName(args.at(kSoundSlotId)) ?? DEFAULT_BUILT_IN_SOUND_NAME;
  const microbit = getMicroBitContextDevice(ctx);
  if (!microbit) {
    handle.resolve(VOID_VALUE);
    return;
  }
  if (hasModifier(args, kImmediatelySlotId)) {
    microbit.speaker.preempt();
  }
  const inBackground = hasModifier(args, kInBackgroundSlotId);
  microbit.speaker.playSoundEmoji(name, ctx.time, (end) => {
    handle.resolve(VOID_VALUE);
    reportDeviceOperationEnding(ctx, { handleId: handle.id, end, inBackground });
  });
  if (inBackground) {
    // The play keeps its speaker lease and resolves on tick time as above;
    // resolving now releases the issuing rule so it does not park on the sound.
    handle.resolve(VOID_VALUE);
  }
}

/**
 * Host actuator: play a built-in sound on the speaker. The optional anonymous
 * sound argument is a `SoundEmoji` value; when absent the target's default
 * sound (`hello`) plays. Asynchronous -- the calling fiber awaits the returned
 * handle and resumes when the sound's nominal duration elapses. With the
 * `immediately` modifier the current speaker lease is preempted at dispatch so
 * the play starts at once; otherwise a play requested while the speaker is
 * busy is dropped. With the `in background` modifier the sound keeps its lease
 * but the handle resolves at dispatch, so the issuing rule continues this
 * round without parking on the playback. A name outside the built-in set is
 * dropped as well: nothing plays and the call resolves at once.
 */
export default {
  ...MicroBitV2HostActions.PlaySound,
  callDef,
  fn: { exec: execPlaySound },
  isAsync: true,
  metadata: {
    label: "play sound",
  },
} satisfies CreateHostActuatorOptions;
