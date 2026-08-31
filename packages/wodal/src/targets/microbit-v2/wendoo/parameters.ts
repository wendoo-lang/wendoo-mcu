import { CoreTypeIds, mkTypeId, NativeType, type ParameterTileInput, param } from "@wendoo/core/app";
import { SOUND_EMOJI_TYPE_ID } from "./built-in-sounds";
import { WodalMicroBitV2ParameterId } from "./tile-ids";

/** TypeId of the `Image` value struct, the data type of the image parameter. */
const IMAGE_TYPE_ID = mkTypeId(NativeType.Struct, "Image");

/**
 * Shared parameter call-spec arg specs.
 *
 * Define each parameter once here and reuse it across sensors and actuators.
 * A new action should reference these shared specs rather than declaring its
 * own parameter tiles. For example, a display-clear-pixel actuator would reuse
 * {@link Param.x} and {@link Param.y}.
 */
export const Param = {
  x: param(WodalMicroBitV2ParameterId.X),
  y: param(WodalMicroBitV2ParameterId.Y),
  brightness: param(WodalMicroBitV2ParameterId.Brightness),
  text: param(WodalMicroBitV2ParameterId.Text, { anonymous: true }),
  image: param(WodalMicroBitV2ParameterId.Image, { anonymous: true }),
  duration: param(WodalMicroBitV2ParameterId.Duration),
  soundEmoji: param(WodalMicroBitV2ParameterId.SoundEmoji, { anonymous: true }),
  volume: param(WodalMicroBitV2ParameterId.Volume),
};

/** Parameter tiles registered once with the module. */
export const MICROBIT_V2_PARAMETERS: readonly ParameterTileInput[] = [
  { id: WodalMicroBitV2ParameterId.X, dataType: CoreTypeIds.Number, label: "x" },
  { id: WodalMicroBitV2ParameterId.Y, dataType: CoreTypeIds.Number, label: "y" },
  { id: WodalMicroBitV2ParameterId.Brightness, dataType: CoreTypeIds.Number, label: "brightness" },
  { id: WodalMicroBitV2ParameterId.Text, dataType: CoreTypeIds.String, label: "text" },
  { id: WodalMicroBitV2ParameterId.Image, dataType: IMAGE_TYPE_ID, label: "image" },
  { id: WodalMicroBitV2ParameterId.Duration, dataType: CoreTypeIds.Number, label: "duration" },
  { id: WodalMicroBitV2ParameterId.Buffer, dataType: CoreTypeIds.Buffer, hidden: true },
  { id: WodalMicroBitV2ParameterId.SoundEmoji, dataType: SOUND_EMOJI_TYPE_ID, label: "sound" },
  { id: WodalMicroBitV2ParameterId.Volume, dataType: CoreTypeIds.Number, label: "volume" },
];
