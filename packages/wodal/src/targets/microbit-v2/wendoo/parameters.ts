import { CoreTypeIds, mkTypeId, NativeType, type ParameterTileInput, param } from "@wendoo/core/app";
import { MICROBIT_LED_MATRIX_SIZE } from "../constants";
import { SOUND_EMOJI_TYPE_ID } from "./built-in-sounds";
import { WodalMicroBitV2ParameterId } from "./tile-ids";

/** TypeId of the `Image` value struct, the data type of the image parameter. */
const IMAGE_TYPE_ID = mkTypeId(NativeType.Struct, "Image");

/** Highest column and row the display holds. */
export const MAX_PIXEL_COORD = MICROBIT_LED_MATRIX_SIZE - 1;

/** Highest LED level a pixel holds; a level past it wraps by modulus. */
export const MAX_BRIGHTNESS = 255;

/**
 * Shared parameter call-spec arg specs.
 *
 * Define each parameter once here and reuse it across sensors and actuators:
 * a spec carries what holds of the parameter wherever it stands -- its name,
 * its unit, and the out-of-range policy that is the parameter's own. An action
 * supplies what an empty slot means to it by spreading the shared spec with its
 * own `default`.
 */
export const Param = {
  x: param(WodalMicroBitV2ParameterId.X, { range: { min: 0, max: MAX_PIXEL_COORD, onExceed: "drop" } }),
  y: param(WodalMicroBitV2ParameterId.Y, { range: { min: 0, max: MAX_PIXEL_COORD, onExceed: "drop" } }),
  brightness: param(WodalMicroBitV2ParameterId.Brightness, {
    range: { min: 0, max: MAX_BRIGHTNESS, onExceed: "wrap" },
  }),
  text: param(WodalMicroBitV2ParameterId.Text, { anonymous: true, name: "text", derived: true }),
  image: param(WodalMicroBitV2ParameterId.Image, { anonymous: true, name: "image" }),
  duration: param(WodalMicroBitV2ParameterId.Duration, { unit: "seconds" }),
  volume: param(WodalMicroBitV2ParameterId.Volume, { unit: "fraction" }),
  soundEmoji: param(WodalMicroBitV2ParameterId.SoundEmoji, { anonymous: true, name: "sound" }),
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
