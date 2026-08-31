/**
 * Stable identifiers for the brain action, modifier, and parameter tiles.
 *
 * These ids are the durable contract between the module and any app that
 * authors brains against it (for example the brain editor visual
 * resolver). The brain catalog keys the tiles by the derived ids returned from
 * `mkSensorTileId`, `mkActuatorTileId`, `mkModifierTileId`, and
 * `mkParameterTileId`.
 */

import type { HostActionIds } from "@wendoo/core/app";

/**
 * Stable numeric funcIds for the microbit-v2 host functions: the native
 * struct methods and the sensor/actuator function entries. `HOST_CALL`
 * dispatches by these values and serialized programs record them verbatim,
 * so an id, once assigned, is never changed or reused. All values are at or
 * above core's `TARGET_FUNC_ID_BASE`. Append new members at the next free
 * id.
 */
export enum MicroBitV2HostFuncId {
  DisplaySetPixelValue = 1024,
  DisplayGetPixelValue = 1025,
  DisplayClear = 1026,
  ButtonIsPressed = 1027,
  TouchButtonIsPressed = 1028,
  TouchButtonGetThreshold = 1029,
  TouchButtonSetThreshold = 1030,
  TouchButtonGetValue = 1031,
  TouchButtonSetValue = 1032,
  SensorButtonA = 1033,
  ActuatorDisplaySetPixel = 1034,
  ActuatorDisplayScroll = 1035,
  SensorButtonB = 1036,
  SensorButtonAB = 1037,
  SensorButtonLogo = 1038,
  AccelerometerGetX = 1039,
  AccelerometerGetY = 1040,
  AccelerometerGetZ = 1041,
  AccelerometerGetPitchRadians = 1042,
  AccelerometerGetRollRadians = 1043,
  AccelerometerGetPitch = 1044,
  AccelerometerGetRoll = 1045,
  AccelerometerGetGesture = 1046,
  SensorGesture = 1047,
  ActuatorDrawImage = 1048,
  DisplayDrawImage = 1049,
  I2CWriteBuffer = 1050,
  I2CReadBuffer = 1051,
  GpioDigitalRead = 1052,
  GpioDigitalWrite = 1053,
  GpioSetPull = 1054,
  GpioServoWrite = 1055,
  SonarDistance = 1056,
  RadioSendNumber = 1057,
  RadioSendString = 1058,
  RadioSendValue = 1059,
  RadioSendBuffer = 1060,
  RadioSendRawBuffer = 1061,
  RadioSetGroup = 1062,
  RadioSetTransmitPower = 1063,
  RadioSetFrequencyBand = 1064,
  RadioReceive = 1065,
  ActuatorRadioSend = 1066,
  SensorRadioReceiveNumber = 1067,
  SensorRadioReceiveString = 1068,
  ActuatorSetRadioGroup = 1069,
  RadioCurrentSeq = 1070,
  GpioAnalogRead = 1071,
  SensorRadioReceiveBuffer = 1072,
  DisplayScrollText = 1073,
  ActuatorPlaySound = 1074,
  AudioPlaySound = 1075,
  ActuatorDisplayClear = 1076,
  DisplayGetLightLevel = 1077,
  SensorLightLevel = 1078,
  ThermometerGetTemperature = 1079,
  SensorTemperature = 1080,
  ActuatorPlayTone = 1081,
  AudioPlayTone = 1082,
}

/**
 * Stable type-atom ids of the microbit-v2 native struct types. Serialized
 * programs reference nominal types by these values, so an id, once assigned,
 * is never changed or reused. All values are at or above core's
 * `TARGET_TYPE_ATOM_BASE`. Append new members at the next free id.
 */
export enum MicroBitV2TypeAtomId {
  MicroBitDisplay = 1024,
  Button = 1025,
  TouchButton = 1026,
  MicroBit = 1027,
  Accelerometer = 1028,
  I2C = 1029,
  GPIO = 1030,
  Sonar = 1031,
  Radio = 1032,
  RadioPacket = 1033,
  RadioPacketList = 1034,
  SoundEmoji = 1035,
  MicroBitAudio = 1036,
  MicroBitThermometer = 1037,
  PlaySoundOptions = 1038,
  DrawImageOptions = 1039,
  ScrollTextOptions = 1040,
  PlayToneOptions = 1041,
}

/**
 * Identity records of the microbit-v2 sensors and actuators, one per host
 * action. The record is the single declaration of each action's key and
 * action id; `fnId` references the action's {@link MicroBitV2HostFuncId}
 * member. Action ids are at or above core's `TARGET_ACTION_ID_BASE` and are
 * permanent once assigned: append new records at the next free action id and
 * never renumber or reuse one.
 */
export const MicroBitV2HostActions = {
  /** Sensor: button A, deriving one button event from the polled press level. */
  ButtonA: { key: "microbit-v2.button-a", actionId: 1024, fnId: MicroBitV2HostFuncId.SensorButtonA },

  /** Actuator: set a single LED pixel brightness on the 5x5 display. */
  DisplaySetPixel: {
    key: "microbit-v2.display-set-pixel",
    actionId: 1025,
    fnId: MicroBitV2HostFuncId.ActuatorDisplaySetPixel,
  },

  /** Actuator: show text on the 5x5 display (scrolled, or static for one character), awaiting the animation. */
  DisplayScroll: {
    key: "microbit-v2.display-scroll",
    actionId: 1026,
    fnId: MicroBitV2HostFuncId.ActuatorDisplayScroll,
  },

  /** Sensor: button B, deriving one button event from the polled press level. */
  ButtonB: { key: "microbit-v2.button-b", actionId: 1027, fnId: MicroBitV2HostFuncId.SensorButtonB },

  /** Sensor: buttons A and B together, pressed only while both are pressed. */
  ButtonAB: { key: "microbit-v2.button-ab", actionId: 1028, fnId: MicroBitV2HostFuncId.SensorButtonAB },

  /** Sensor: the capacitive touch logo, deriving events from the polled touch level. */
  ButtonLogo: {
    key: "microbit-v2.button-logo",
    actionId: 1029,
    fnId: MicroBitV2HostFuncId.SensorButtonLogo,
  },

  /** Sensor: an accelerometer gesture, true while the polled gesture matches the chosen modifier. */
  Gesture: { key: "microbit-v2.gesture", actionId: 1030, fnId: MicroBitV2HostFuncId.SensorGesture },

  /** Actuator: paste an image to the 5x5 display, optionally holding it for a duration. */
  DrawImage: {
    key: "microbit-v2.draw-image",
    actionId: 1031,
    fnId: MicroBitV2HostFuncId.ActuatorDrawImage,
  },

  /** Actuator: broadcast the optional value (or the WHEN-result) as a radio packet. */
  RadioSend: {
    key: "microbit-v2.radio-send",
    actionId: 1032,
    fnId: MicroBitV2HostFuncId.ActuatorRadioSend,
  },

  /** Sensor: the next received NUMBER / DOUBLE packet, delivering its numeric value. */
  RadioReceiveNumber: {
    key: "microbit-v2.radio-receive-number",
    actionId: 1033,
    fnId: MicroBitV2HostFuncId.SensorRadioReceiveNumber,
  },

  /** Sensor: the next received STRING packet, delivering its string value. */
  RadioReceiveString: {
    key: "microbit-v2.radio-receive-string",
    actionId: 1034,
    fnId: MicroBitV2HostFuncId.SensorRadioReceiveString,
  },

  /** Actuator: set the radio group (0-255). */
  SetRadioGroup: {
    key: "microbit-v2.set-radio-group",
    actionId: 1035,
    fnId: MicroBitV2HostFuncId.ActuatorSetRadioGroup,
  },

  /** Sensor: the next received BUFFER packet, delivering its raw payload Buffer. */
  RadioReceiveBuffer: {
    key: "microbit-v2.radio-receive-buffer",
    actionId: 1036,
    fnId: MicroBitV2HostFuncId.SensorRadioReceiveBuffer,
  },

  /** Actuator: play a built-in sound on the speaker, awaiting its nominal duration. */
  PlaySound: {
    key: "microbit-v2.play-sound",
    actionId: 1037,
    fnId: MicroBitV2HostFuncId.ActuatorPlaySound,
  },

  /** Actuator: blank the 5x5 display, cancelling any held display lease. */
  DisplayClear: {
    key: "microbit-v2.display-clear",
    actionId: 1038,
    fnId: MicroBitV2HostFuncId.ActuatorDisplayClear,
  },

  /** Sensor: the ambient light level read off the LED matrix, 0 (dark) to 255 (bright). */
  LightLevel: {
    key: "microbit-v2.light-level",
    actionId: 1039,
    fnId: MicroBitV2HostFuncId.SensorLightLevel,
  },

  /** Sensor: the die temperature in whole degrees Celsius; signed. */
  Temperature: {
    key: "microbit-v2.temperature",
    actionId: 1040,
    fnId: MicroBitV2HostFuncId.SensorTemperature,
  },

  /** Actuator: play a plain constant-pitch tone on the speaker, awaiting its duration. */
  PlayTone: {
    key: "microbit-v2.play-tone",
    actionId: 1041,
    fnId: MicroBitV2HostFuncId.ActuatorPlayTone,
  },
} as const satisfies Record<string, HostActionIds>;

/**
 * Modifier tile ids: the optional words a tile carries to select a variant of
 * its behavior. A button sensor takes at most one button-event modifier, and an
 * absent one selects `pressed`; a gesture sensor takes at most one gesture
 * modifier, and an absent one selects `shake`; the display and speaker
 * actuators take the lease modifiers.
 */
export const WodalMicroBitV2ModifierId = {
  /** Report the released-to-pressed edge. */
  Pressed: "microbit-v2.pressed",

  /** Report the pressed-to-released edge. */
  Released: "microbit-v2.released",

  /** Report a release whose preceding press was shorter than the long-click threshold. */
  Click: "microbit-v2.click",

  /** Report a press beginning within the double-click window after a click. */
  DoubleClick: "microbit-v2.double-click",

  /** Report a release whose preceding press was at least the long-click threshold. */
  LongClick: "microbit-v2.long-click",

  /** Report every tick the button is currently pressed (a level, not an edge). */
  Held: "microbit-v2.held",

  /** Match the shake gesture. */
  Shake: "microbit-v2.shake",

  /** Match the tilt-up gesture. */
  TiltUp: "microbit-v2.tilt-up",

  /** Match the tilt-down gesture. */
  TiltDown: "microbit-v2.tilt-down",

  /** Match the tilt-left gesture. */
  TiltLeft: "microbit-v2.tilt-left",

  /** Match the tilt-right gesture. */
  TiltRight: "microbit-v2.tilt-right",

  /** Match the face-up gesture. */
  FaceUp: "microbit-v2.face-up",

  /** Match the face-down gesture. */
  FaceDown: "microbit-v2.face-down",

  /** Match the freefall gesture. */
  Freefall: "microbit-v2.freefall",

  /** Preempt the current display or speaker lease so the operation runs at once. */
  Immediately: "microbit-v2.immediately",

  /** Run the operation under its display or speaker lease without the issuing rule awaiting it. */
  InBackground: "microbit-v2.in-background",

  /** Sound a tone as a square wave. */
  Square: "microbit-v2.square",

  /** Sound a tone as a sawtooth wave. */
  Sawtooth: "microbit-v2.sawtooth",

  /** Sound a tone as a sine wave. */
  Sine: "microbit-v2.sine",

  /** Sound a tone as a triangle wave. */
  Triangle: "microbit-v2.triangle",
} as const;

/** Parameter tile ids consumed by the actuators. */
export const WodalMicroBitV2ParameterId = {
  /** Display column index, 0 to 4. */
  X: "microbit-v2.x",

  /** Display row index, 0 to 4. */
  Y: "microbit-v2.y",

  /** LED brightness, 0 to 255. */
  Brightness: "microbit-v2.brightness",

  /** Text to scroll across the display. */
  Text: "microbit-v2.text",

  /** Image to paste onto the display. */
  Image: "microbit-v2.image",

  /** Seconds a temporal draw holds the display. */
  Duration: "microbit-v2.duration",

  /** Byte buffer filling an anonymous Buffer value slot. */
  Buffer: "microbit-v2.buffer",

  /** Built-in sound to play on the speaker. */
  SoundEmoji: "microbit-v2.sound-emoji",

  /** Tone volume as a fraction of full, 0 to 1. */
  Volume: "microbit-v2.volume",
} as const;
