import {
  CoreControlFlowId,
  CoreHostActions,
  CoreLiteralFactoryId,
  CoreOpId,
  CoreTypeIds,
  CoreVariableFactoryId,
  mkActuatorTileId,
  mkControlFlowTileId,
  mkLiteralFactoryTileId,
  mkLiteralTileId,
  mkModifierTileId,
  mkOperatorTileId,
  mkOutputTileId,
  mkParameterTileId,
  mkSensorTileId,
  mkVariableFactoryTileId,
} from "@wendoo/core/app";
import type { TileVisual } from "@wendoo/ui";
import { staticAssetUrl } from "@wendoo/ui";
import { WODAL_SHARED_TYPE_IDS } from "@wendoo/wodal";
import {
  MicroBitV2HostActions,
  SOUND_EMOJI_TYPE_ID,
  WodalMicroBitV2ModifierId,
  WodalMicroBitV2ParameterId,
} from "@wendoo/wodal/targets/microbit-v2";

/** Base URL of the app's tile and data-type icon assets. */
export const ICON_BASE = staticAssetUrl("assets/brain/icons");

/** URL of a named icon under {@link ICON_BASE}. */
function icon(name: string): string {
  return `${ICON_BASE}/${name}.svg`;
}

/**
 * Display visuals for the tiles microbit-sim ships, keyed by tile id. Every
 * entry carries the tile's icon URL. An entry sets a label only where the app
 * presents the tile under a different word than its catalog metadata authors.
 */
// biome-ignore format: uniform one-liner-per-entry style
export const tileVisuals = new Map<string, Partial<TileVisual>>([
  // Operators
  [mkOperatorTileId(CoreOpId.And), { iconUrl: icon("and") }],
  [mkOperatorTileId(CoreOpId.Or), { iconUrl: icon("or") }],
  [mkOperatorTileId(CoreOpId.Not), { iconUrl: icon("not") }],
  [mkOperatorTileId(CoreOpId.Add), { iconUrl: icon("plus") }],
  [mkOperatorTileId(CoreOpId.Subtract), { iconUrl: icon("minus") }],
  [mkOperatorTileId(CoreOpId.Multiply), { iconUrl: icon("multiply") }],
  [mkOperatorTileId(CoreOpId.Divide), { iconUrl: icon("divide") }],
  [mkOperatorTileId(CoreOpId.Negate), { iconUrl: icon("negative") }],
  [mkOperatorTileId(CoreOpId.EqualTo), { iconUrl: icon("equals") }],
  [mkOperatorTileId(CoreOpId.NotEqualTo), { iconUrl: icon("not_equal") }],
  [mkOperatorTileId(CoreOpId.LessThan), { iconUrl: icon("less_than") }],
  [mkOperatorTileId(CoreOpId.LessThanOrEqualTo), { iconUrl: icon("less_than_or_equal_to") }],
  [mkOperatorTileId(CoreOpId.GreaterThan), { iconUrl: icon("greater_than") }],
  [mkOperatorTileId(CoreOpId.GreaterThanOrEqualTo), { iconUrl: icon("greater_than_or_equal_to") }],
  [mkOperatorTileId(CoreOpId.Assign), { iconUrl: icon("assign") }],
  // Control Flow
  [mkControlFlowTileId(CoreControlFlowId.OpenParen), { iconUrl: icon("open-paren") }],
  [mkControlFlowTileId(CoreControlFlowId.CloseParen), { iconUrl: icon("close-paren") }],
  // Variable Factories
  [mkVariableFactoryTileId(CoreVariableFactoryId.Boolean), { label: "create a boolean variable", iconUrl: icon("var-factory-boolean") }],
  [mkVariableFactoryTileId(CoreVariableFactoryId.Number), { label: "create a number variable", iconUrl: icon("var-factory-number") }],
  [mkVariableFactoryTileId(CoreVariableFactoryId.String), { label: "create a text variable", iconUrl: icon("var-factory-text") }],
  // Literal Factories
  [mkLiteralFactoryTileId(CoreLiteralFactoryId.Number), { iconUrl: icon("lit-factory-number") }],
  [mkLiteralFactoryTileId(CoreLiteralFactoryId.String), { iconUrl: icon("lit-factory-text") }],
  // Core literals
  [mkLiteralTileId(CoreTypeIds.Boolean, "true"), { iconUrl: icon("literal-true") }],
  [mkLiteralTileId(CoreTypeIds.Boolean, "false"), { iconUrl: icon("literal-false") }],
  [mkLiteralTileId(CoreTypeIds.Nil, "nil"), { iconUrl: icon("literal-nil") }],
  // Built-in image literals
  [mkLiteralTileId(WODAL_SHARED_TYPE_IDS.Image, "heart"), { iconUrl: icon("image-heart") }],
  [mkLiteralTileId(WODAL_SHARED_TYPE_IDS.Image, "happy"), { iconUrl: icon("image-happy") }],
  [mkLiteralTileId(WODAL_SHARED_TYPE_IDS.Image, "sad"), { iconUrl: icon("image-sad") }],
  [mkLiteralTileId(WODAL_SHARED_TYPE_IDS.Image, "arrow-north"), { iconUrl: icon("image-arrow-north") }],
  [mkLiteralTileId(WODAL_SHARED_TYPE_IDS.Image, "arrow-south"), { iconUrl: icon("image-arrow-south") }],
  [mkLiteralTileId(WODAL_SHARED_TYPE_IDS.Image, "arrow-east"), { iconUrl: icon("image-arrow-east") }],
  [mkLiteralTileId(WODAL_SHARED_TYPE_IDS.Image, "arrow-west"), { iconUrl: icon("image-arrow-west") }],
  // Built-in sound literals
  [mkLiteralTileId(SOUND_EMOJI_TYPE_ID, "giggle"), { iconUrl: icon("literal-sound") }],
  [mkLiteralTileId(SOUND_EMOJI_TYPE_ID, "happy"), { iconUrl: icon("literal-sound") }],
  [mkLiteralTileId(SOUND_EMOJI_TYPE_ID, "hello"), { iconUrl: icon("literal-sound") }],
  [mkLiteralTileId(SOUND_EMOJI_TYPE_ID, "mysterious"), { iconUrl: icon("literal-sound") }],
  [mkLiteralTileId(SOUND_EMOJI_TYPE_ID, "sad"), { iconUrl: icon("literal-sound") }],
  [mkLiteralTileId(SOUND_EMOJI_TYPE_ID, "slide"), { iconUrl: icon("literal-sound") }],
  [mkLiteralTileId(SOUND_EMOJI_TYPE_ID, "soaring"), { iconUrl: icon("literal-sound") }],
  [mkLiteralTileId(SOUND_EMOJI_TYPE_ID, "spring"), { iconUrl: icon("literal-sound") }],
  [mkLiteralTileId(SOUND_EMOJI_TYPE_ID, "twinkle"), { iconUrl: icon("literal-sound") }],
  [mkLiteralTileId(SOUND_EMOJI_TYPE_ID, "yawn"), { iconUrl: icon("literal-sound") }],
  // Sensors
  [mkSensorTileId(CoreHostActions.Random.key), { iconUrl: icon("sensor-random") }],
  [mkSensorTileId(CoreHostActions.OnPageEntered.key), { iconUrl: icon("sensor-on-page-entered") }],
  [mkSensorTileId(CoreHostActions.Timeout.key), { iconUrl: icon("sensor-timeout") }],
  [mkSensorTileId(CoreHostActions.CurrentPage.key), { iconUrl: icon("page") }],
  [mkSensorTileId(CoreHostActions.PreviousPage.key), { iconUrl: icon("page2") }],
  [mkSensorTileId(CoreHostActions.Otherwise.key), { iconUrl: icon("sensor-otherwise") }],
  [mkSensorTileId(MicroBitV2HostActions.ButtonA.key), { iconUrl: icon("sensor-button-a") }],
  [mkSensorTileId(MicroBitV2HostActions.ButtonB.key), { iconUrl: icon("sensor-button-b") }],
  [mkSensorTileId(MicroBitV2HostActions.ButtonAB.key), { iconUrl: icon("sensor-button-ab") }],
  [mkSensorTileId(MicroBitV2HostActions.ButtonLogo.key), { iconUrl: icon("sensor-button-logo") }],
  [mkSensorTileId(MicroBitV2HostActions.Gesture.key), { iconUrl: icon("sensor-gesture") }],
  [mkSensorTileId(MicroBitV2HostActions.LightLevel.key), { iconUrl: icon("sensor-light-level") }],
  [mkSensorTileId(MicroBitV2HostActions.Temperature.key), { iconUrl: icon("sensor-temperature") }],
  [mkSensorTileId(MicroBitV2HostActions.RadioReceiveNumber.key), { iconUrl: icon("sensor-radio-receive-number") }],
  [mkSensorTileId(MicroBitV2HostActions.RadioReceiveString.key), { iconUrl: icon("sensor-radio-receive-string") }],
  [mkSensorTileId(MicroBitV2HostActions.RadioReceiveBuffer.key), { iconUrl: icon("sensor-radio-receive-buffer") }],
  // Actuators
  [mkActuatorTileId(CoreHostActions.SwitchPage.key), { iconUrl: icon("actuator-switch-page") }],
  [mkActuatorTileId(MicroBitV2HostActions.RadioSend.key), { iconUrl: icon("actuator-radio-send") }],
  [mkActuatorTileId(MicroBitV2HostActions.SetRadioGroup.key), { iconUrl: icon("actuator-set-radio-group") }],
  [mkActuatorTileId(MicroBitV2HostActions.DisplaySetPixel.key), { iconUrl: icon("actuator-display-set-pixel") }],
  [mkActuatorTileId(MicroBitV2HostActions.DisplayScroll.key), { iconUrl: icon("actuator-display-text") }],
  [mkActuatorTileId(MicroBitV2HostActions.DisplayClear.key), { iconUrl: icon("actuator-display-clear") }],
  [mkActuatorTileId(MicroBitV2HostActions.DrawImage.key), { iconUrl: icon("actuator-draw-image") }],
  [mkActuatorTileId(MicroBitV2HostActions.PlaySound.key), { iconUrl: icon("actuator-play-sound") }],
  [mkActuatorTileId(MicroBitV2HostActions.PlayTone.key), { iconUrl: icon("actuator-play-tone") }],
  // Sensor outputs
  [mkOutputTileId(CoreTypeIds.Number, "value"), { iconUrl: icon("output-number-value") }],
  [mkOutputTileId(CoreTypeIds.Number, "rssi"), { iconUrl: icon("output-number-rssi") }],
  [mkOutputTileId(CoreTypeIds.String, "value"), { iconUrl: icon("output-string-value") }],
  [mkOutputTileId(CoreTypeIds.Buffer, "value"), { iconUrl: icon("output-buffer-value") }],
  // Modifiers
  [mkModifierTileId(WodalMicroBitV2ModifierId.Pressed), { iconUrl: icon("modifier-pressed") }],
  [mkModifierTileId(WodalMicroBitV2ModifierId.Released), { iconUrl: icon("modifier-released") }],
  [mkModifierTileId(WodalMicroBitV2ModifierId.Click), { iconUrl: icon("modifier-click") }],
  [mkModifierTileId(WodalMicroBitV2ModifierId.DoubleClick), { iconUrl: icon("modifier-double-click") }],
  [mkModifierTileId(WodalMicroBitV2ModifierId.LongClick), { iconUrl: icon("modifier-long-click") }],
  [mkModifierTileId(WodalMicroBitV2ModifierId.Held), { iconUrl: icon("modifier-held") }],
  [mkModifierTileId(WodalMicroBitV2ModifierId.Shake), { iconUrl: icon("modifier-shake") }],
  [mkModifierTileId(WodalMicroBitV2ModifierId.TiltUp), { iconUrl: icon("modifier-tilt-up") }],
  [mkModifierTileId(WodalMicroBitV2ModifierId.TiltDown), { iconUrl: icon("modifier-tilt-down") }],
  [mkModifierTileId(WodalMicroBitV2ModifierId.TiltLeft), { iconUrl: icon("modifier-tilt-left") }],
  [mkModifierTileId(WodalMicroBitV2ModifierId.TiltRight), { iconUrl: icon("modifier-tilt-right") }],
  [mkModifierTileId(WodalMicroBitV2ModifierId.FaceUp), { iconUrl: icon("modifier-face-up") }],
  [mkModifierTileId(WodalMicroBitV2ModifierId.FaceDown), { iconUrl: icon("modifier-face-down") }],
  [mkModifierTileId(WodalMicroBitV2ModifierId.Freefall), { iconUrl: icon("modifier-freefall") }],
  [mkModifierTileId(WodalMicroBitV2ModifierId.Immediately), { iconUrl: icon("modifier-immediately") }],
  [mkModifierTileId(WodalMicroBitV2ModifierId.InBackground), { iconUrl: icon("modifier-in-background") }],
  [mkModifierTileId(WodalMicroBitV2ModifierId.Square), { iconUrl: icon("modifier-square") }],
  [mkModifierTileId(WodalMicroBitV2ModifierId.Sawtooth), { iconUrl: icon("modifier-sawtooth") }],
  [mkModifierTileId(WodalMicroBitV2ModifierId.Sine), { iconUrl: icon("modifier-sine") }],
  [mkModifierTileId(WodalMicroBitV2ModifierId.Triangle), { iconUrl: icon("modifier-triangle") }],
  // Parameters
  [mkParameterTileId(WodalMicroBitV2ParameterId.X), { iconUrl: icon("parameter-x") }],
  [mkParameterTileId(WodalMicroBitV2ParameterId.Y), { iconUrl: icon("parameter-y") }],
  [mkParameterTileId(WodalMicroBitV2ParameterId.Brightness), { iconUrl: icon("parameter-brightness") }],
  [mkParameterTileId(WodalMicroBitV2ParameterId.Text), { iconUrl: icon("parameter-text") }],
  [mkParameterTileId(WodalMicroBitV2ParameterId.Image), { iconUrl: icon("parameter-image") }],
  [mkParameterTileId(WodalMicroBitV2ParameterId.Duration), { iconUrl: icon("parameter-duration") }],
  [mkParameterTileId(WodalMicroBitV2ParameterId.SoundEmoji), { iconUrl: icon("parameter-sound") }],
  [mkParameterTileId(WodalMicroBitV2ParameterId.Volume), { iconUrl: icon("parameter-volume") }],
]);
