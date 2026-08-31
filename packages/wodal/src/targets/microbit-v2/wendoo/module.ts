import { stream } from "@wendoo/core";
import {
  type AsyncHandle,
  BrainTileLiteralDef,
  ContextTypeIds,
  CoreTypeIds,
  createHostActuator,
  createHostSensor,
  type ExecutionContext,
  extractNumberValue,
  extractStringValue,
  isBooleanValue,
  isNilValue,
  isStructValue,
  List,
  mkCallDef,
  mkClosedStructValue,
  mkListValue,
  mkNativeStructValue,
  mkNumberValue,
  mkStringValue,
  mkTypeId,
  NativeType,
  NIL_VALUE,
  type ReadonlyList,
  type StructValue,
  type Value,
  VOID_VALUE,
  type WendooModule,
  type WendooModuleApi,
} from "@wendoo/core/app";
import { bufferByteAt, bufferLength, isBufferValue, mkBufferValue } from "@wendoo/core/runtime";
import { Accelerometer } from "../../../core/accelerometer";
import { Button } from "../../../core/button";
import { Gpio } from "../../../core/gpio";
import { I2CBus } from "../../../core/i2c-bus";
import { toNonNegativeInteger } from "../../../core/numeric";
import {
  RADIO_RAW_PACKET_TYPE,
  Radio,
  RadioPacketType,
  type RadioSendRecord,
  type ReceivedRadioPacket,
  radioNumberIsInteger,
} from "../../../core/radio";
import { SensorDriver } from "../../../core/sensor-driver";
import { Thermometer } from "../../../core/thermometer";
import { TouchButton } from "../../../core/touch-button";
import { WODAL_SHARED_TYPE_IDS } from "../../../wendoo/shared-type-ids";
import { MicroBit } from "../microbit";
import { MicroBitDisplay } from "../microbit-display";
import { findToneWaveform, MicroBitSpeaker, mkSpeakerToneCommand, type SpeakerToneWaveform } from "../microbit-speaker";
import { buttonABSensor, buttonASensor, buttonBSensor, buttonLogoSensor } from "./actions/button-sensor";
import displayClearActuator from "./actions/display-clear";
import displayDrawActuator, { clipImage, DEFAULT_DURATION_MS, DEFAULT_IMAGE } from "./actions/display-draw";
import { brightnessToPort, pixelCoordToPort } from "./actions/display-pixel-conversion";
import displayScrollActuator from "./actions/display-scroll";
import displaySetPixelActuator from "./actions/display-set-pixel";
import { gestureSensor } from "./actions/gesture-sensor";
import { lightLevelSensor } from "./actions/light-level-sensor";
import playSoundActuator from "./actions/play-sound";
import playToneActuator, {
  DEFAULT_DURATION_SECONDS,
  DEFAULT_FREQUENCY_HZ,
  DEFAULT_VOLUME,
  DEFAULT_WAVEFORM,
} from "./actions/play-tone";
import {
  radioReceiveBufferSensor,
  radioReceiveNumberSensor,
  radioReceiveOutputTiles,
  radioReceiveStringSensor,
} from "./actions/radio-receive";
import radioSendActuator from "./actions/radio-send";
import setRadioGroupActuator from "./actions/set-radio-group";
import { temperatureSensor } from "./actions/temperature-sensor";
import { BUILT_IN_IMAGES, builtInImageStructValue } from "./built-in-images";
import { BUILT_IN_SOUNDS, builtInSoundStructValue, SOUND_EMOJI_TYPE_ID, SoundEmojiField } from "./built-in-sounds";
import { getMicroBitContextDevice } from "./context";
import { SCROLL_DEFAULT_DELAY_MS, scrollDurationMs } from "./display-scroll";
import { MICROBIT_V2_MODIFIERS } from "./modifiers";
import { MICROBIT_V2_PARAMETERS } from "./parameters";
import { MicroBitV2HostFuncId, MicroBitV2TypeAtomId } from "./tile-ids";

/** Wendoo module ID for the WODAL profile. */
export const WODAL_MICROBIT_V2_MODULE_ID = "wendoo.microbit-v2";

/** Wendoo type IDs registered by the WODAL profile. */
export const WODAL_MICROBIT_V2_TYPE_IDS = {
  /** Native-backed aggregate for the simulated device. */
  MicroBit: mkTypeId(NativeType.Struct, "MicroBit"),

  /** Native-backed display facade for the simulated device. */
  MicroBitDisplay: mkTypeId(NativeType.Struct, "MicroBitDisplay"),

  /** Native-backed button facade for the simulated device. */
  Button: mkTypeId(NativeType.Struct, "Button"),

  /** Native-backed touch button facade for the simulated device. */
  TouchButton: mkTypeId(NativeType.Struct, "TouchButton"),

  /** Native-backed accelerometer facade for the simulated device. */
  Accelerometer: mkTypeId(NativeType.Struct, "Accelerometer"),

  /** Native-backed I2C bus facade for the simulated device. */
  I2C: mkTypeId(NativeType.Struct, "I2C"),

  /** Native-backed GPIO pin facade for the simulated device. */
  GPIO: mkTypeId(NativeType.Struct, "GPIO"),

  /** Native-backed sonar facade for the simulated device's background sensor driver. */
  Sonar: mkTypeId(NativeType.Struct, "Sonar"),

  /** Native-backed radio facade for the simulated device. */
  Radio: mkTypeId(NativeType.Struct, "Radio"),

  /** Native-backed speaker audio facade for the simulated device. */
  MicroBitAudio: mkTypeId(NativeType.Struct, "MicroBitAudio"),

  /** Native-backed thermometer facade for the simulated device. */
  Thermometer: mkTypeId(NativeType.Struct, "Thermometer"),

  /** Options struct for `MicroBitAudio.playSound`: the two optional lease flags. */
  PlaySoundOptions: mkTypeId(NativeType.Struct, "PlaySoundOptions"),

  /** Options struct for `MicroBitAudio.playTone`: the optional tone shape and the two lease flags. */
  PlayToneOptions: mkTypeId(NativeType.Struct, "PlayToneOptions"),

  /** Options struct for `MicroBitDisplay.drawImage`: the optional hold duration and two lease flags. */
  DrawImageOptions: mkTypeId(NativeType.Struct, "DrawImageOptions"),

  /** Options struct for `MicroBitDisplay.scrollText`: the two optional lease flags. */
  ScrollTextOptions: mkTypeId(NativeType.Struct, "ScrollTextOptions"),

  /** Value struct describing one received radio packet (drain-all element). */
  RadioPacket: mkTypeId(NativeType.Struct, "RadioPacket"),

  /** List of {@link RadioPacket}, the drain-all receive result type. */
  RadioPacketList: mkTypeId(NativeType.List, "RadioPacketList"),
} as const;

/**
 * Field ids of the `RadioPacket` value struct, in declaration order. Each is the
 * field's storage slot in the closed struct value the drain-all receive builds.
 */
enum RadioPacketField {
  Seq = 0,
  Type = 1,
  Value = 2,
  Name = 3,
  Text = 4,
  Buffer = 5,
  Rssi = 6,
  Serial = 7,
  Time = 8,
}

/**
 * Numeric field ids for the `MicroBit` struct. Each value is the field's durable
 * id and storage slot, and is the single source for both the registered
 * `fieldIndex` and the `fieldGetter` dispatch.
 */
export enum MicroBitField {
  Display = 0,
  ButtonA = 1,
  ButtonB = 2,
  Logo = 3,
  Accelerometer = 4,
  I2C = 5,
  GPIO = 6,
  Sonar = 7,
  Radio = 8,
  Audio = 9,
  Thermometer = 10,
}

/**
 * Field ids of the `PlaySoundOptions` and `ScrollTextOptions` option structs,
 * whose two optional lease flags share this layout in declaration order.
 */
enum LeaseOptionsField {
  Immediately = 0,
  InBackground = 1,
}

/**
 * Field ids of the `DrawImageOptions` option struct in declaration order: the
 * optional hold duration ahead of the two lease flags.
 */
enum DrawImageOptionsField {
  Duration = 0,
  Immediately = 1,
  InBackground = 2,
}

/**
 * Field ids of the `PlayToneOptions` option struct in declaration order: the
 * three optional tone controls ahead of the two lease flags.
 */
enum PlayToneOptionsField {
  Duration = 0,
  Volume = 1,
  Waveform = 2,
  Immediately = 3,
  InBackground = 4,
}

/**
 * Field id of the `microbit` field this profile adds to the core `Context`
 * struct. Core owns Context ids 0-5; device extensions start at 6.
 */
export const CONTEXT_MICROBIT_FIELD_ID = 6;

/** The value at `fieldIndex` of an options struct arg, or nil when the arg is absent or not a struct. */
function optionField(options: Value | undefined, fieldIndex: number): Value {
  return isStructValue(options) && options.v !== undefined ? (options.v.at(fieldIndex) ?? NIL_VALUE) : NIL_VALUE;
}

/** True when the boolean field at `fieldIndex` of an options struct arg is present and true. */
function optionFlag(options: Value | undefined, fieldIndex: number): boolean {
  const field = optionField(options, fieldIndex);
  return isBooleanValue(field) && field.v;
}

/** Creates the Wendoo module for the WODAL profile. */
export function createMicroBitV2Module(): WendooModule {
  return {
    id: WODAL_MICROBIT_V2_MODULE_ID,
    install(api: WendooModuleApi): void {
      registerMicroBitTypes(api);
      registerMicroBitDisplayFunctions(api);
      registerButtonFunctions(api);
      registerAccelerometerFunctions(api);
      registerThermometerFunctions(api);
      registerI2CFunctions(api);
      registerGPIOFunctions(api);
      registerSonarFunctions(api);
      registerRadioFunctions(api);
      registerAudioFunctions(api);
      registerBrainTiles(api);
    },
  };
}

function registerMicroBitTypes(api: WendooModuleApi): void {
  const { types } = api.brainServices.runtime;

  types.addStructType("MicroBitDisplay", {
    atomId: MicroBitV2TypeAtomId.MicroBitDisplay,
    fields: List.empty(),
    fieldGetter: () => undefined,
    methods: List.from([
      {
        name: "setPixelValue",
        params: List.from([
          { name: "x", typeId: CoreTypeIds.Number },
          { name: "y", typeId: CoreTypeIds.Number },
          { name: "brightness", typeId: CoreTypeIds.Number },
        ]),
        returnTypeId: CoreTypeIds.Void,
      },
      {
        name: "getPixelValue",
        params: List.from([
          { name: "x", typeId: CoreTypeIds.Number },
          { name: "y", typeId: CoreTypeIds.Number },
        ]),
        returnTypeId: CoreTypeIds.Number,
      },
      {
        name: "clear",
        params: List.empty(),
        returnTypeId: CoreTypeIds.Void,
      },
      {
        name: "drawImage",
        params: List.from([
          { name: "image", typeId: WODAL_SHARED_TYPE_IDS.Image },
          { name: "options", typeId: WODAL_MICROBIT_V2_TYPE_IDS.DrawImageOptions, optional: true },
        ]),
        returnTypeId: CoreTypeIds.Void,
        isAsync: true,
      },
      {
        name: "scrollText",
        params: List.from([
          { name: "text", typeId: CoreTypeIds.String },
          { name: "options", typeId: WODAL_MICROBIT_V2_TYPE_IDS.ScrollTextOptions, optional: true },
        ]),
        returnTypeId: CoreTypeIds.Void,
        isAsync: true,
      },
      {
        name: "getLightLevel",
        params: List.empty(),
        returnTypeId: CoreTypeIds.Number,
      },
    ]),
  });

  types.addStructType("Button", {
    atomId: MicroBitV2TypeAtomId.Button,
    fields: List.empty(),
    fieldGetter: () => undefined,
    methods: List.from([
      {
        name: "isPressed",
        params: List.empty(),
        returnTypeId: CoreTypeIds.Number,
      },
    ]),
  });

  types.addStructType("TouchButton", {
    atomId: MicroBitV2TypeAtomId.TouchButton,
    fields: List.empty(),
    fieldGetter: () => undefined,
    methods: List.from([
      {
        name: "isPressed",
        params: List.empty(),
        returnTypeId: CoreTypeIds.Number,
      },
      {
        name: "getThreshold",
        params: List.empty(),
        returnTypeId: CoreTypeIds.Number,
      },
      {
        name: "setThreshold",
        params: List.from([{ name: "threshold", typeId: CoreTypeIds.Number }]),
        returnTypeId: CoreTypeIds.Void,
      },
      {
        name: "getValue",
        params: List.empty(),
        returnTypeId: CoreTypeIds.Number,
      },
      {
        name: "setValue",
        params: List.from([{ name: "value", typeId: CoreTypeIds.Number }]),
        returnTypeId: CoreTypeIds.Void,
      },
    ]),
  });

  types.addStructType("Accelerometer", {
    atomId: MicroBitV2TypeAtomId.Accelerometer,
    fields: List.empty(),
    fieldGetter: () => undefined,
    methods: List.from([
      { name: "getX", params: List.empty(), returnTypeId: CoreTypeIds.Number },
      { name: "getY", params: List.empty(), returnTypeId: CoreTypeIds.Number },
      { name: "getZ", params: List.empty(), returnTypeId: CoreTypeIds.Number },
      { name: "getPitchRadians", params: List.empty(), returnTypeId: CoreTypeIds.Number },
      { name: "getRollRadians", params: List.empty(), returnTypeId: CoreTypeIds.Number },
      { name: "getPitch", params: List.empty(), returnTypeId: CoreTypeIds.Number },
      { name: "getRoll", params: List.empty(), returnTypeId: CoreTypeIds.Number },
      { name: "getGesture", params: List.empty(), returnTypeId: CoreTypeIds.Number },
    ]),
  });

  types.addStructType("Thermometer", {
    atomId: MicroBitV2TypeAtomId.MicroBitThermometer,
    fields: List.empty(),
    fieldGetter: () => undefined,
    methods: List.from([{ name: "getTemperature", params: List.empty(), returnTypeId: CoreTypeIds.Number }]),
  });

  types.addStructType("I2C", {
    atomId: MicroBitV2TypeAtomId.I2C,
    fields: List.empty(),
    fieldGetter: () => undefined,
    methods: List.from([
      {
        name: "writeBuffer",
        params: List.from([
          { name: "address", typeId: CoreTypeIds.Number },
          { name: "data", typeId: CoreTypeIds.Buffer },
        ]),
        returnTypeId: CoreTypeIds.Number,
      },
      {
        name: "readBuffer",
        params: List.from([
          { name: "address", typeId: CoreTypeIds.Number },
          { name: "length", typeId: CoreTypeIds.Number },
        ]),
        returnTypeId: CoreTypeIds.Buffer,
      },
    ]),
  });

  types.addStructType("GPIO", {
    atomId: MicroBitV2TypeAtomId.GPIO,
    fields: List.empty(),
    fieldGetter: () => undefined,
    methods: List.from([
      {
        name: "digitalRead",
        params: List.from([{ name: "pin", typeId: CoreTypeIds.Number }]),
        returnTypeId: CoreTypeIds.Number,
      },
      {
        name: "digitalWrite",
        params: List.from([
          { name: "pin", typeId: CoreTypeIds.Number },
          { name: "value", typeId: CoreTypeIds.Number },
        ]),
        returnTypeId: CoreTypeIds.Number,
      },
      {
        name: "setPull",
        params: List.from([
          { name: "pin", typeId: CoreTypeIds.Number },
          { name: "mode", typeId: CoreTypeIds.Number },
        ]),
        returnTypeId: CoreTypeIds.Number,
      },
      {
        name: "servoWrite",
        params: List.from([
          { name: "pin", typeId: CoreTypeIds.Number },
          { name: "angle", typeId: CoreTypeIds.Number },
        ]),
        returnTypeId: CoreTypeIds.Number,
      },
      {
        name: "analogRead",
        params: List.from([{ name: "pin", typeId: CoreTypeIds.Number }]),
        returnTypeId: CoreTypeIds.Number,
      },
    ]),
  });

  types.addStructType("Sonar", {
    atomId: MicroBitV2TypeAtomId.Sonar,
    fields: List.empty(),
    fieldGetter: () => undefined,
    methods: List.from([
      {
        name: "distance",
        params: List.from([
          { name: "trig", typeId: CoreTypeIds.Number },
          { name: "echo", typeId: CoreTypeIds.Number },
        ]),
        returnTypeId: CoreTypeIds.Number,
      },
    ]),
  });

  types.addStructType("RadioPacket", {
    atomId: MicroBitV2TypeAtomId.RadioPacket,
    fields: List.from([
      { name: "seq", typeId: CoreTypeIds.Number, fieldIndex: RadioPacketField.Seq },
      { name: "type", typeId: CoreTypeIds.Number, fieldIndex: RadioPacketField.Type },
      { name: "value", typeId: CoreTypeIds.Number, fieldIndex: RadioPacketField.Value },
      { name: "name", typeId: CoreTypeIds.String, fieldIndex: RadioPacketField.Name },
      { name: "text", typeId: CoreTypeIds.String, fieldIndex: RadioPacketField.Text },
      { name: "buffer", typeId: CoreTypeIds.Buffer, fieldIndex: RadioPacketField.Buffer },
      { name: "rssi", typeId: CoreTypeIds.Number, fieldIndex: RadioPacketField.Rssi },
      { name: "serial", typeId: CoreTypeIds.Number, fieldIndex: RadioPacketField.Serial },
      { name: "time", typeId: CoreTypeIds.Number, fieldIndex: RadioPacketField.Time },
    ]),
  });

  types.addListType("RadioPacketList", {
    atomId: MicroBitV2TypeAtomId.RadioPacketList,
    elementTypeId: WODAL_MICROBIT_V2_TYPE_IDS.RadioPacket,
  });

  types.addStructType("SoundEmoji", {
    atomId: MicroBitV2TypeAtomId.SoundEmoji,
    fields: List.from([{ name: "name", typeId: CoreTypeIds.String, fieldIndex: SoundEmojiField.Name }]),
  });

  types.addStructType("PlaySoundOptions", {
    atomId: MicroBitV2TypeAtomId.PlaySoundOptions,
    fields: List.from([
      { name: "immediately", typeId: CoreTypeIds.Boolean, fieldIndex: LeaseOptionsField.Immediately, optional: true },
      { name: "inBackground", typeId: CoreTypeIds.Boolean, fieldIndex: LeaseOptionsField.InBackground, optional: true },
    ]),
  });

  types.addStructType("PlayToneOptions", {
    atomId: MicroBitV2TypeAtomId.PlayToneOptions,
    fields: List.from([
      { name: "duration", typeId: CoreTypeIds.Number, fieldIndex: PlayToneOptionsField.Duration, optional: true },
      { name: "volume", typeId: CoreTypeIds.Number, fieldIndex: PlayToneOptionsField.Volume, optional: true },
      { name: "waveform", typeId: CoreTypeIds.String, fieldIndex: PlayToneOptionsField.Waveform, optional: true },
      {
        name: "immediately",
        typeId: CoreTypeIds.Boolean,
        fieldIndex: PlayToneOptionsField.Immediately,
        optional: true,
      },
      {
        name: "inBackground",
        typeId: CoreTypeIds.Boolean,
        fieldIndex: PlayToneOptionsField.InBackground,
        optional: true,
      },
    ]),
  });

  types.addStructType("DrawImageOptions", {
    atomId: MicroBitV2TypeAtomId.DrawImageOptions,
    fields: List.from([
      { name: "duration", typeId: CoreTypeIds.Number, fieldIndex: DrawImageOptionsField.Duration, optional: true },
      {
        name: "immediately",
        typeId: CoreTypeIds.Boolean,
        fieldIndex: DrawImageOptionsField.Immediately,
        optional: true,
      },
      {
        name: "inBackground",
        typeId: CoreTypeIds.Boolean,
        fieldIndex: DrawImageOptionsField.InBackground,
        optional: true,
      },
    ]),
  });

  types.addStructType("ScrollTextOptions", {
    atomId: MicroBitV2TypeAtomId.ScrollTextOptions,
    fields: List.from([
      { name: "immediately", typeId: CoreTypeIds.Boolean, fieldIndex: LeaseOptionsField.Immediately, optional: true },
      { name: "inBackground", typeId: CoreTypeIds.Boolean, fieldIndex: LeaseOptionsField.InBackground, optional: true },
    ]),
  });

  types.addStructType("Radio", {
    atomId: MicroBitV2TypeAtomId.Radio,
    fields: List.empty(),
    fieldGetter: () => undefined,
    methods: List.from([
      {
        name: "sendNumber",
        params: List.from([{ name: "value", typeId: CoreTypeIds.Number }]),
        returnTypeId: CoreTypeIds.Void,
      },
      {
        name: "sendString",
        params: List.from([{ name: "text", typeId: CoreTypeIds.String }]),
        returnTypeId: CoreTypeIds.Void,
      },
      {
        name: "sendValue",
        params: List.from([
          { name: "name", typeId: CoreTypeIds.String },
          { name: "value", typeId: CoreTypeIds.Number },
        ]),
        returnTypeId: CoreTypeIds.Void,
      },
      {
        name: "sendBuffer",
        params: List.from([{ name: "buffer", typeId: CoreTypeIds.Buffer }]),
        returnTypeId: CoreTypeIds.Void,
      },
      {
        name: "sendRawBuffer",
        params: List.from([{ name: "buffer", typeId: CoreTypeIds.Buffer }]),
        returnTypeId: CoreTypeIds.Void,
      },
      {
        name: "setGroup",
        params: List.from([{ name: "group", typeId: CoreTypeIds.Number }]),
        returnTypeId: CoreTypeIds.Void,
      },
      {
        name: "setTransmitPower",
        params: List.from([{ name: "power", typeId: CoreTypeIds.Number }]),
        returnTypeId: CoreTypeIds.Void,
      },
      {
        name: "setFrequencyBand",
        params: List.from([{ name: "band", typeId: CoreTypeIds.Number }]),
        returnTypeId: CoreTypeIds.Void,
      },
      {
        name: "receive",
        params: List.from([{ name: "since", typeId: CoreTypeIds.Number }]),
        returnTypeId: WODAL_MICROBIT_V2_TYPE_IDS.RadioPacketList,
      },
      {
        name: "currentSeq",
        params: List.empty(),
        returnTypeId: CoreTypeIds.Number,
      },
    ]),
  });

  types.addStructType("MicroBitAudio", {
    atomId: MicroBitV2TypeAtomId.MicroBitAudio,
    fields: List.empty(),
    fieldGetter: () => undefined,
    methods: List.from([
      {
        name: "playSound",
        params: List.from([
          { name: "sound", typeId: CoreTypeIds.String },
          { name: "options", typeId: WODAL_MICROBIT_V2_TYPE_IDS.PlaySoundOptions, optional: true },
        ]),
        returnTypeId: CoreTypeIds.Void,
        isAsync: true,
      },
      {
        name: "playTone",
        params: List.from([
          { name: "frequency", typeId: CoreTypeIds.Number, optional: true },
          { name: "options", typeId: WODAL_MICROBIT_V2_TYPE_IDS.PlayToneOptions, optional: true },
        ]),
        returnTypeId: CoreTypeIds.Void,
        isAsync: true,
      },
    ]),
  });

  types.addStructType("MicroBit", {
    atomId: MicroBitV2TypeAtomId.MicroBit,
    fields: List.from([
      { name: "display", typeId: WODAL_MICROBIT_V2_TYPE_IDS.MicroBitDisplay, fieldIndex: MicroBitField.Display },
      { name: "buttonA", typeId: WODAL_MICROBIT_V2_TYPE_IDS.Button, fieldIndex: MicroBitField.ButtonA },
      { name: "buttonB", typeId: WODAL_MICROBIT_V2_TYPE_IDS.Button, fieldIndex: MicroBitField.ButtonB },
      { name: "logo", typeId: WODAL_MICROBIT_V2_TYPE_IDS.TouchButton, fieldIndex: MicroBitField.Logo },
      {
        name: "accelerometer",
        typeId: WODAL_MICROBIT_V2_TYPE_IDS.Accelerometer,
        fieldIndex: MicroBitField.Accelerometer,
      },
      { name: "i2c", typeId: WODAL_MICROBIT_V2_TYPE_IDS.I2C, fieldIndex: MicroBitField.I2C },
      { name: "gpio", typeId: WODAL_MICROBIT_V2_TYPE_IDS.GPIO, fieldIndex: MicroBitField.GPIO },
      { name: "sonar", typeId: WODAL_MICROBIT_V2_TYPE_IDS.Sonar, fieldIndex: MicroBitField.Sonar },
      { name: "radio", typeId: WODAL_MICROBIT_V2_TYPE_IDS.Radio, fieldIndex: MicroBitField.Radio },
      { name: "audio", typeId: WODAL_MICROBIT_V2_TYPE_IDS.MicroBitAudio, fieldIndex: MicroBitField.Audio },
      {
        name: "thermometer",
        typeId: WODAL_MICROBIT_V2_TYPE_IDS.Thermometer,
        fieldIndex: MicroBitField.Thermometer,
      },
    ]),
    fieldGetter: (source, fieldId) => {
      const microbit = getNativeMicroBit(source);
      if (!microbit) {
        return NIL_VALUE;
      }
      switch (fieldId) {
        case MicroBitField.Display:
          return mkNativeStructValue(WODAL_MICROBIT_V2_TYPE_IDS.MicroBitDisplay, microbit.display);
        case MicroBitField.ButtonA:
          return mkNativeStructValue(WODAL_MICROBIT_V2_TYPE_IDS.Button, microbit.buttonA);
        case MicroBitField.ButtonB:
          return mkNativeStructValue(WODAL_MICROBIT_V2_TYPE_IDS.Button, microbit.buttonB);
        case MicroBitField.Logo:
          return mkNativeStructValue(WODAL_MICROBIT_V2_TYPE_IDS.TouchButton, microbit.logo);
        case MicroBitField.Accelerometer:
          return mkNativeStructValue(WODAL_MICROBIT_V2_TYPE_IDS.Accelerometer, microbit.accelerometer);
        case MicroBitField.I2C:
          return mkNativeStructValue(WODAL_MICROBIT_V2_TYPE_IDS.I2C, microbit.i2c);
        case MicroBitField.GPIO:
          return mkNativeStructValue(WODAL_MICROBIT_V2_TYPE_IDS.GPIO, microbit.gpio);
        case MicroBitField.Sonar:
          return mkNativeStructValue(WODAL_MICROBIT_V2_TYPE_IDS.Sonar, microbit.sensorDriver);
        case MicroBitField.Radio:
          return mkNativeStructValue(WODAL_MICROBIT_V2_TYPE_IDS.Radio, microbit.radio);
        case MicroBitField.Audio:
          return mkNativeStructValue(WODAL_MICROBIT_V2_TYPE_IDS.MicroBitAudio, microbit.speaker);
        case MicroBitField.Thermometer:
          return mkNativeStructValue(WODAL_MICROBIT_V2_TYPE_IDS.Thermometer, microbit.thermometer);
        default:
          return undefined;
      }
    },
  });

  types.addStructFields(
    ContextTypeIds.Context,
    List.from([
      { name: "microbit", typeId: WODAL_MICROBIT_V2_TYPE_IDS.MicroBit, fieldIndex: CONTEXT_MICROBIT_FIELD_ID },
    ]),
    (_source, fieldId, ctx) => {
      if (fieldId !== CONTEXT_MICROBIT_FIELD_ID) {
        return undefined;
      }
      const microbit = getMicroBitContextDevice(ctx);
      return microbit ? mkNativeStructValue(WODAL_MICROBIT_V2_TYPE_IDS.MicroBit, microbit) : NIL_VALUE;
    }
  );
}

function registerMicroBitDisplayFunctions(api: WendooModuleApi): void {
  const emptyCallDef = mkCallDef({ type: "bag", items: [] });

  api.registerFunction({
    id: MicroBitV2HostFuncId.DisplaySetPixelValue,
    name: "MicroBitDisplay.setPixelValue",
    isAsync: false,
    fn: {
      exec: (_ctx: ExecutionContext, args: ReadonlyList<Value>) => {
        getDisplayReceiver(args)?.setPixelValue(
          pixelCoordToPort(numberArg(args, 1)),
          pixelCoordToPort(numberArg(args, 2)),
          brightnessToPort(numberArg(args, 3))
        );
        return VOID_VALUE;
      },
    },
    callDef: emptyCallDef,
  });

  api.registerFunction({
    id: MicroBitV2HostFuncId.DisplayGetPixelValue,
    name: "MicroBitDisplay.getPixelValue",
    isAsync: false,
    fn: {
      exec: (_ctx: ExecutionContext, args: ReadonlyList<Value>) => {
        const brightness = getDisplayReceiver(args)?.getPixelValue(numberArg(args, 1), numberArg(args, 2)) ?? 0;
        return mkNumberValue(brightness);
      },
    },
    callDef: emptyCallDef,
  });

  api.registerFunction({
    id: MicroBitV2HostFuncId.DisplayClear,
    name: "MicroBitDisplay.clear",
    isAsync: false,
    fn: {
      exec: (_ctx: ExecutionContext, args: ReadonlyList<Value>) => {
        getDisplayReceiver(args)?.clear();
        return VOID_VALUE;
      },
    },
    callDef: emptyCallDef,
  });

  api.registerFunction({
    id: MicroBitV2HostFuncId.DisplayDrawImage,
    name: "MicroBitDisplay.drawImage",
    isAsync: true,
    fn: {
      exec: (ctx: ExecutionContext, args: ReadonlyList<Value>, handle: AsyncHandle) => {
        const display = getDisplayReceiver(args);
        if (!display) {
          handle.resolve(VOID_VALUE);
          return;
        }
        const options = args.at(2);
        // The `immediately` flag preempts the current display lease at dispatch,
        // so the draw runs at once even when the display is busy.
        if (optionFlag(options, DrawImageOptionsField.Immediately)) {
          display.preempt();
        }
        const imageArg = args.at(1);
        const clipped = (imageArg !== undefined ? clipImage(imageArg) : undefined) ?? DEFAULT_IMAGE;
        const durationSeconds = extractNumberValue(optionField(options, DrawImageOptionsField.Duration));
        // Convert the seconds argument to whole ms at f32 precision, matching the device.
        const durationMs =
          durationSeconds === undefined
            ? DEFAULT_DURATION_MS
            : toNonNegativeInteger(Math.fround(durationSeconds * 1000));
        // This host function draws a single image, leased as a one-frame sequence.
        display.drawImage([clipped], durationMs, ctx.time, () => handle.resolve(VOID_VALUE));
        // The `inBackground` flag keeps the draw's tick-time lease but resolves the
        // handle now, releasing the caller so it does not park on the hold. It is
        // distinct from a zero duration, which takes no lease at all.
        if (optionFlag(options, DrawImageOptionsField.InBackground)) {
          handle.resolve(VOID_VALUE);
        }
      },
    },
    callDef: emptyCallDef,
  });

  api.registerFunction({
    id: MicroBitV2HostFuncId.DisplayGetLightLevel,
    name: "MicroBitDisplay.getLightLevel",
    isAsync: false,
    fn: {
      exec: (_ctx: ExecutionContext, args: ReadonlyList<Value>) =>
        mkNumberValue(getDisplayReceiver(args)?.getLightLevel() ?? 0),
    },
    callDef: emptyCallDef,
  });

  api.registerFunction({
    id: MicroBitV2HostFuncId.DisplayScrollText,
    name: "MicroBitDisplay.scrollText",
    isAsync: true,
    fn: {
      exec: (ctx: ExecutionContext, args: ReadonlyList<Value>, handle: AsyncHandle) => {
        const display = getDisplayReceiver(args);
        if (!display) {
          handle.resolve(VOID_VALUE);
          return;
        }
        const options = args.at(2);
        // The `immediately` flag preempts the current display lease at dispatch,
        // so the show runs at once even when the display is busy.
        if (optionFlag(options, LeaseOptionsField.Immediately)) {
          display.preempt();
        }
        const text = extractStringValue(args.at(1)) ?? "";
        const durationMs = scrollDurationMs(text.length, SCROLL_DEFAULT_DELAY_MS);
        display.scrollText(text, durationMs, ctx.time, () => handle.resolve(VOID_VALUE));
        // The `inBackground` flag keeps the show's tick-time lease but resolves the
        // handle now, releasing the caller so it does not park on the animation.
        if (optionFlag(options, LeaseOptionsField.InBackground)) {
          handle.resolve(VOID_VALUE);
        }
      },
    },
    callDef: emptyCallDef,
  });
}

function registerButtonFunctions(api: WendooModuleApi): void {
  const emptyCallDef = mkCallDef({ type: "bag", items: [] });

  const isPressedRegistrations: ReadonlyArray<{ typeName: string; id: MicroBitV2HostFuncId }> = [
    { typeName: "Button", id: MicroBitV2HostFuncId.ButtonIsPressed },
    { typeName: "TouchButton", id: MicroBitV2HostFuncId.TouchButtonIsPressed },
  ];
  for (const { typeName, id } of isPressedRegistrations) {
    api.registerFunction({
      id,
      name: `${typeName}.isPressed`,
      isAsync: false,
      fn: {
        exec: (_ctx: ExecutionContext, args: ReadonlyList<Value>) =>
          mkNumberValue(getButtonReceiver(args)?.isPressed() ?? 0),
      },
      callDef: emptyCallDef,
    });
  }

  api.registerFunction({
    id: MicroBitV2HostFuncId.TouchButtonGetThreshold,
    name: "TouchButton.getThreshold",
    isAsync: false,
    fn: {
      exec: (_ctx: ExecutionContext, args: ReadonlyList<Value>) =>
        mkNumberValue(getTouchButtonReceiver(args)?.getThreshold() ?? 0),
    },
    callDef: emptyCallDef,
  });

  api.registerFunction({
    id: MicroBitV2HostFuncId.TouchButtonSetThreshold,
    name: "TouchButton.setThreshold",
    isAsync: false,
    fn: {
      exec: (_ctx: ExecutionContext, args: ReadonlyList<Value>) => {
        getTouchButtonReceiver(args)?.setThreshold(numberArg(args, 1));
        return VOID_VALUE;
      },
    },
    callDef: emptyCallDef,
  });

  api.registerFunction({
    id: MicroBitV2HostFuncId.TouchButtonGetValue,
    name: "TouchButton.getValue",
    isAsync: false,
    fn: {
      exec: (_ctx: ExecutionContext, args: ReadonlyList<Value>) =>
        mkNumberValue(getTouchButtonReceiver(args)?.getValue() ?? 0),
    },
    callDef: emptyCallDef,
  });

  api.registerFunction({
    id: MicroBitV2HostFuncId.TouchButtonSetValue,
    name: "TouchButton.setValue",
    isAsync: false,
    fn: {
      exec: (ctx: ExecutionContext, args: ReadonlyList<Value>) => {
        getTouchButtonReceiver(args)?.setValue(numberArg(args, 1), ctx.time);
        return VOID_VALUE;
      },
    },
    callDef: emptyCallDef,
  });
}

function registerAccelerometerFunctions(api: WendooModuleApi): void {
  const emptyCallDef = mkCallDef({ type: "bag", items: [] });

  const reads: ReadonlyArray<{ name: string; id: MicroBitV2HostFuncId; read: (a: Accelerometer) => number }> = [
    { name: "getX", id: MicroBitV2HostFuncId.AccelerometerGetX, read: (a) => a.getX() },
    { name: "getY", id: MicroBitV2HostFuncId.AccelerometerGetY, read: (a) => a.getY() },
    { name: "getZ", id: MicroBitV2HostFuncId.AccelerometerGetZ, read: (a) => a.getZ() },
    {
      name: "getPitchRadians",
      id: MicroBitV2HostFuncId.AccelerometerGetPitchRadians,
      read: (a) => a.getPitchRadians(),
    },
    { name: "getRollRadians", id: MicroBitV2HostFuncId.AccelerometerGetRollRadians, read: (a) => a.getRollRadians() },
    { name: "getPitch", id: MicroBitV2HostFuncId.AccelerometerGetPitch, read: (a) => a.getPitch() },
    { name: "getRoll", id: MicroBitV2HostFuncId.AccelerometerGetRoll, read: (a) => a.getRoll() },
    { name: "getGesture", id: MicroBitV2HostFuncId.AccelerometerGetGesture, read: (a) => a.getGesture() },
  ];
  for (const { name, id, read } of reads) {
    api.registerFunction({
      id,
      name: `Accelerometer.${name}`,
      isAsync: false,
      fn: {
        exec: (_ctx: ExecutionContext, args: ReadonlyList<Value>) => {
          const receiver = getAccelerometerReceiver(args);
          return mkNumberValue(receiver ? read(receiver) : 0);
        },
      },
      callDef: emptyCallDef,
    });
  }
}

function registerThermometerFunctions(api: WendooModuleApi): void {
  const emptyCallDef = mkCallDef({ type: "bag", items: [] });

  api.registerFunction({
    id: MicroBitV2HostFuncId.ThermometerGetTemperature,
    name: "Thermometer.getTemperature",
    isAsync: false,
    fn: {
      exec: (_ctx: ExecutionContext, args: ReadonlyList<Value>) =>
        mkNumberValue(getThermometerReceiver(args)?.getTemperature() ?? 0),
    },
    callDef: emptyCallDef,
  });
}

function registerI2CFunctions(api: WendooModuleApi): void {
  const emptyCallDef = mkCallDef({ type: "bag", items: [] });

  api.registerFunction({
    id: MicroBitV2HostFuncId.I2CWriteBuffer,
    name: "I2C.writeBuffer",
    isAsync: false,
    fn: {
      exec: (_ctx: ExecutionContext, args: ReadonlyList<Value>) => {
        const bus = getI2CReceiver(args);
        const address = toNonNegativeInteger(numberArg(args, 1));
        const bytes = bufferArgBytes(args.at(2));
        return mkNumberValue(bus ? bus.write(address, bytes) : 0);
      },
    },
    callDef: emptyCallDef,
  });

  api.registerFunction({
    id: MicroBitV2HostFuncId.I2CReadBuffer,
    name: "I2C.readBuffer",
    isAsync: false,
    fn: {
      exec: (_ctx: ExecutionContext, args: ReadonlyList<Value>) => {
        const bus = getI2CReceiver(args);
        const address = toNonNegativeInteger(numberArg(args, 1));
        const length = toNonNegativeInteger(numberArg(args, 2));
        const bytes = bus ? bus.read(address, length) : new Uint8Array(0);
        return mkBufferValue(stream.byteArrayFromUint8Array(bytes));
      },
    },
    callDef: emptyCallDef,
  });
}

function registerGPIOFunctions(api: WendooModuleApi): void {
  const emptyCallDef = mkCallDef({ type: "bag", items: [] });

  api.registerFunction({
    id: MicroBitV2HostFuncId.GpioDigitalRead,
    name: "GPIO.digitalRead",
    isAsync: false,
    fn: {
      exec: (_ctx: ExecutionContext, args: ReadonlyList<Value>) => {
        const gpio = getGPIOReceiver(args);
        const pin = toNonNegativeInteger(numberArg(args, 1));
        return mkNumberValue(gpio ? gpio.digitalRead(pin) : 0);
      },
    },
    callDef: emptyCallDef,
  });

  api.registerFunction({
    id: MicroBitV2HostFuncId.GpioDigitalWrite,
    name: "GPIO.digitalWrite",
    isAsync: false,
    fn: {
      exec: (_ctx: ExecutionContext, args: ReadonlyList<Value>) => {
        const gpio = getGPIOReceiver(args);
        const pin = toNonNegativeInteger(numberArg(args, 1));
        const value = toNonNegativeInteger(numberArg(args, 2));
        return mkNumberValue(gpio ? gpio.digitalWrite(pin, value) : 0);
      },
    },
    callDef: emptyCallDef,
  });

  api.registerFunction({
    id: MicroBitV2HostFuncId.GpioSetPull,
    name: "GPIO.setPull",
    isAsync: false,
    fn: {
      exec: (_ctx: ExecutionContext, args: ReadonlyList<Value>) => {
        const gpio = getGPIOReceiver(args);
        const pin = toNonNegativeInteger(numberArg(args, 1));
        const mode = toNonNegativeInteger(numberArg(args, 2));
        return mkNumberValue(gpio ? gpio.setPull(pin, mode) : 0);
      },
    },
    callDef: emptyCallDef,
  });

  api.registerFunction({
    id: MicroBitV2HostFuncId.GpioServoWrite,
    name: "GPIO.servoWrite",
    isAsync: false,
    fn: {
      exec: (_ctx: ExecutionContext, args: ReadonlyList<Value>) => {
        const gpio = getGPIOReceiver(args);
        const pin = toNonNegativeInteger(numberArg(args, 1));
        const angle = toNonNegativeInteger(numberArg(args, 2));
        return mkNumberValue(gpio ? gpio.setServo(pin, angle) : 0);
      },
    },
    callDef: emptyCallDef,
  });

  api.registerFunction({
    id: MicroBitV2HostFuncId.GpioAnalogRead,
    name: "GPIO.analogRead",
    isAsync: false,
    fn: {
      exec: (_ctx: ExecutionContext, args: ReadonlyList<Value>) => {
        const gpio = getGPIOReceiver(args);
        const pin = toNonNegativeInteger(numberArg(args, 1));
        return mkNumberValue(gpio ? gpio.analogRead(pin) : 0);
      },
    },
    callDef: emptyCallDef,
  });
}

function registerSonarFunctions(api: WendooModuleApi): void {
  const emptyCallDef = mkCallDef({ type: "bag", items: [] });

  api.registerFunction({
    id: MicroBitV2HostFuncId.SonarDistance,
    name: "Sonar.distance",
    isAsync: false,
    fn: {
      exec: (_ctx: ExecutionContext, args: ReadonlyList<Value>) => {
        const driver = getSonarReceiver(args);
        const trig = toNonNegativeInteger(numberArg(args, 1));
        const echo = toNonNegativeInteger(numberArg(args, 2));
        return mkNumberValue(driver ? driver.sonarDistance(trig, echo) : 0);
      },
    },
    callDef: emptyCallDef,
  });
}

const EMPTY_BYTES = new Uint8Array(0);

function registerRadioFunctions(api: WendooModuleApi): void {
  const emptyCallDef = mkCallDef({ type: "bag", items: [] });

  const sendRecord = (args: ReadonlyList<Value>, record: RadioSendRecord): Value => {
    const radio = getRadioReceiver(args);
    if (radio) {
      radio.send(record);
    }
    return VOID_VALUE;
  };

  api.registerFunction({
    id: MicroBitV2HostFuncId.RadioSendNumber,
    name: "Radio.sendNumber",
    isAsync: false,
    fn: {
      exec: (_ctx: ExecutionContext, args: ReadonlyList<Value>) => {
        const radio = getRadioReceiver(args);
        const value = numberArg(args, 1);
        return sendRecord(args, {
          type: radioNumberIsInteger(value) ? RadioPacketType.Number : RadioPacketType.Double,
          group: radio ? radio.group : 0,
          value,
          name: "",
          text: "",
          bytes: EMPTY_BYTES,
        });
      },
    },
    callDef: emptyCallDef,
  });

  api.registerFunction({
    id: MicroBitV2HostFuncId.RadioSendString,
    name: "Radio.sendString",
    isAsync: false,
    fn: {
      exec: (_ctx: ExecutionContext, args: ReadonlyList<Value>) => {
        const radio = getRadioReceiver(args);
        return sendRecord(args, {
          type: RadioPacketType.String,
          group: radio ? radio.group : 0,
          value: 0,
          name: "",
          text: extractStringValue(args.at(1)) ?? "",
          bytes: EMPTY_BYTES,
        });
      },
    },
    callDef: emptyCallDef,
  });

  api.registerFunction({
    id: MicroBitV2HostFuncId.RadioSendValue,
    name: "Radio.sendValue",
    isAsync: false,
    fn: {
      exec: (_ctx: ExecutionContext, args: ReadonlyList<Value>) => {
        const radio = getRadioReceiver(args);
        const value = numberArg(args, 2);
        return sendRecord(args, {
          type: radioNumberIsInteger(value) ? RadioPacketType.Value : RadioPacketType.DoubleValue,
          group: radio ? radio.group : 0,
          value,
          name: extractStringValue(args.at(1)) ?? "",
          text: "",
          bytes: EMPTY_BYTES,
        });
      },
    },
    callDef: emptyCallDef,
  });

  api.registerFunction({
    id: MicroBitV2HostFuncId.RadioSendBuffer,
    name: "Radio.sendBuffer",
    isAsync: false,
    fn: {
      exec: (_ctx: ExecutionContext, args: ReadonlyList<Value>) => {
        const radio = getRadioReceiver(args);
        return sendRecord(args, {
          type: RadioPacketType.Buffer,
          group: radio ? radio.group : 0,
          value: 0,
          name: "",
          text: "",
          bytes: bufferArgBytes(args.at(1)),
        });
      },
    },
    callDef: emptyCallDef,
  });

  api.registerFunction({
    id: MicroBitV2HostFuncId.RadioSendRawBuffer,
    name: "Radio.sendRawBuffer",
    isAsync: false,
    fn: {
      exec: (_ctx: ExecutionContext, args: ReadonlyList<Value>) => {
        const radio = getRadioReceiver(args);
        return sendRecord(args, {
          type: RADIO_RAW_PACKET_TYPE,
          group: radio ? radio.group : 0,
          value: 0,
          name: "",
          text: "",
          bytes: bufferArgBytes(args.at(1)),
        });
      },
    },
    callDef: emptyCallDef,
  });

  const configFns: ReadonlyArray<{ id: MicroBitV2HostFuncId; name: string; apply: (radio: Radio, n: number) => void }> =
    [
      { id: MicroBitV2HostFuncId.RadioSetGroup, name: "Radio.setGroup", apply: (radio, n) => radio.setGroup(n) },
      {
        id: MicroBitV2HostFuncId.RadioSetTransmitPower,
        name: "Radio.setTransmitPower",
        apply: (radio, n) => radio.setTransmitPower(n),
      },
      {
        id: MicroBitV2HostFuncId.RadioSetFrequencyBand,
        name: "Radio.setFrequencyBand",
        apply: (radio, n) => radio.setFrequencyBand(n),
      },
    ];
  for (const { id, name, apply } of configFns) {
    api.registerFunction({
      id,
      name,
      isAsync: false,
      fn: {
        exec: (_ctx: ExecutionContext, args: ReadonlyList<Value>) => {
          const radio = getRadioReceiver(args);
          if (radio) {
            apply(radio, numberArg(args, 1));
          }
          return VOID_VALUE;
        },
      },
      callDef: emptyCallDef,
    });
  }

  api.registerFunction({
    id: MicroBitV2HostFuncId.RadioReceive,
    name: "Radio.receive",
    isAsync: false,
    fn: {
      exec: (_ctx: ExecutionContext, args: ReadonlyList<Value>) => {
        const radio = getRadioReceiver(args);
        if (!radio) {
          return mkListValue(WODAL_MICROBIT_V2_TYPE_IDS.RadioPacketList, List.empty());
        }
        const since = toNonNegativeInteger(numberArg(args, 1));
        return mkListValue(
          WODAL_MICROBIT_V2_TYPE_IDS.RadioPacketList,
          List.from(radio.drainAfter(since).map(radioPacketStructValue))
        );
      },
    },
    callDef: emptyCallDef,
  });

  api.registerFunction({
    id: MicroBitV2HostFuncId.RadioCurrentSeq,
    name: "Radio.currentSeq",
    isAsync: false,
    fn: {
      exec: (_ctx: ExecutionContext, args: ReadonlyList<Value>) => {
        const radio = getRadioReceiver(args);
        return mkNumberValue(radio ? radio.headSequence() : 0);
      },
    },
    callDef: emptyCallDef,
  });
}

function registerAudioFunctions(api: WendooModuleApi): void {
  const emptyCallDef = mkCallDef({ type: "bag", items: [] });

  api.registerFunction({
    id: MicroBitV2HostFuncId.AudioPlaySound,
    name: "MicroBitAudio.playSound",
    isAsync: true,
    fn: {
      exec: (ctx: ExecutionContext, args: ReadonlyList<Value>, handle: AsyncHandle) => {
        const speaker = getAudioReceiver(args);
        if (!speaker) {
          handle.resolve(VOID_VALUE);
          return;
        }
        // A non-string or absent argument reads as an empty name, which the
        // speaker treats as a name outside the built-in set: a silent no-op.
        const name = extractStringValue(args.at(1)) ?? "";
        const options = args.at(2);
        // The `immediately` flag preempts the current speaker lease at dispatch,
        // before the new play is examined, so an unknown name still preempts.
        if (optionFlag(options, LeaseOptionsField.Immediately)) {
          speaker.preempt();
        }
        speaker.playSoundEmoji(name, ctx.time, () => handle.resolve(VOID_VALUE));
        // The `inBackground` flag keeps the play's tick-time lease but resolves
        // the handle now, releasing the caller so it does not park on playback.
        if (optionFlag(options, LeaseOptionsField.InBackground)) {
          handle.resolve(VOID_VALUE);
        }
      },
    },
    callDef: emptyCallDef,
  });

  api.registerFunction({
    id: MicroBitV2HostFuncId.AudioPlayTone,
    name: "MicroBitAudio.playTone",
    isAsync: true,
    fn: {
      exec: (ctx: ExecutionContext, args: ReadonlyList<Value>, handle: AsyncHandle) => {
        const speaker = getAudioReceiver(args);
        if (!speaker) {
          handle.resolve(VOID_VALUE);
          return;
        }
        const options = args.at(2);
        // The `immediately` flag preempts the current speaker lease at dispatch,
        // before the new tone is examined, so an unknown waveform still preempts.
        if (optionFlag(options, PlayToneOptionsField.Immediately)) {
          speaker.preempt();
        }
        const waveform = requestedWaveform(optionField(options, PlayToneOptionsField.Waveform));
        if (waveform === undefined) {
          handle.resolve(VOID_VALUE);
          return;
        }
        const durationSeconds = finiteNumber(
          optionField(options, PlayToneOptionsField.Duration),
          DEFAULT_DURATION_SECONDS
        );
        // Convert the seconds argument to whole ms at f32 precision, matching the device.
        const command = mkSpeakerToneCommand(
          waveform,
          finiteNumber(args.at(1), DEFAULT_FREQUENCY_HZ),
          Math.round(Math.fround(durationSeconds * 1000)),
          finiteNumber(optionField(options, PlayToneOptionsField.Volume), DEFAULT_VOLUME)
        );
        speaker.playTone(command, ctx.time, () => handle.resolve(VOID_VALUE));
        // The `inBackground` flag keeps the tone's tick-time lease but resolves
        // the handle now, releasing the caller so it does not park on the tone.
        if (optionFlag(options, PlayToneOptionsField.InBackground)) {
          handle.resolve(VOID_VALUE);
        }
      },
    },
    callDef: emptyCallDef,
  });
}

/**
 * The wave shape the `waveform` option field names: the default when the field
 * is absent, and undefined when it names a shape the port does not sound.
 */
function requestedWaveform(field: Value): SpeakerToneWaveform | undefined {
  return isNilValue(field) ? DEFAULT_WAVEFORM : findToneWaveform(extractStringValue(field) ?? "");
}

/** The number `value` holds, or `fallback` when it is absent, nil, or non-finite. */
function finiteNumber(value: Value | undefined, fallback: number): number {
  const number = extractNumberValue(value);
  return number === undefined || !Number.isFinite(number) ? fallback : number;
}

/** Builds a `RadioPacket` value struct from a received packet, in field-index order. */
function radioPacketStructValue(packet: ReceivedRadioPacket): Value {
  return mkClosedStructValue(
    WODAL_MICROBIT_V2_TYPE_IDS.RadioPacket,
    List.from([
      mkNumberValue(packet.seq),
      mkNumberValue(packet.type),
      mkNumberValue(packet.value),
      mkStringValue(packet.name),
      mkStringValue(packet.text),
      mkBufferValue(stream.byteArrayFromUint8Array(packet.bytes)),
      mkNumberValue(packet.rssi),
      mkNumberValue(packet.serial),
      mkNumberValue(packet.time),
    ])
  );
}

function registerBrainTiles(api: WendooModuleApi): void {
  api.registerHostSensor(createHostSensor(buttonASensor));
  api.registerHostSensor(createHostSensor(buttonBSensor));
  api.registerHostSensor(createHostSensor(buttonABSensor));
  api.registerHostSensor(createHostSensor(buttonLogoSensor));
  api.registerHostSensor(createHostSensor(gestureSensor));
  api.registerHostSensor(createHostSensor(lightLevelSensor));
  api.registerHostSensor(createHostSensor(temperatureSensor));
  api.registerHostSensor(createHostSensor(radioReceiveNumberSensor));
  api.registerHostSensor(createHostSensor(radioReceiveStringSensor));
  api.registerHostSensor(createHostSensor(radioReceiveBufferSensor));
  for (const outputTile of radioReceiveOutputTiles) {
    api.registerTile(outputTile);
  }
  api.registerHostActuator(createHostActuator(radioSendActuator));
  api.registerHostActuator(createHostActuator(setRadioGroupActuator));
  api.registerHostActuator(createHostActuator(displaySetPixelActuator));
  api.registerHostActuator(createHostActuator(displayScrollActuator));
  api.registerHostActuator(createHostActuator(displayDrawActuator));
  api.registerHostActuator(createHostActuator(displayClearActuator));
  api.registerHostActuator(createHostActuator(playSoundActuator));
  api.registerHostActuator(createHostActuator(playToneActuator));
  api.registerModifiers(MICROBIT_V2_MODIFIERS);
  api.registerParameters(MICROBIT_V2_PARAMETERS);
  registerBuiltInImageTiles(api);
  registerBuiltInSoundTiles(api);
}

/**
 * Registers a surface-1 `Image` literal tile for each built-in icon. Each tile
 * carries the icon's baked `Image` struct value (see {@link builtInImageStructValue}):
 * placing it in a `draw image` image slot bakes that constant into the program.
 * The tiles are catalog-provided and referenced by id, so they do not persist
 * their struct value into a saved brain.
 */
function registerBuiltInImageTiles(api: WendooModuleApi): void {
  for (const def of BUILT_IN_IMAGES) {
    api.registerTile(
      new BrainTileLiteralDef(
        WODAL_SHARED_TYPE_IDS.Image,
        builtInImageStructValue(def),
        { valueLabel: def.name, persist: false, metadata: { label: def.label, language: { form: def.label } } },
        api.brainServices
      )
    );
  }
}

/**
 * Registers a surface-1 `SoundEmoji` literal tile for each built-in sound. Each
 * tile carries the sound's baked `SoundEmoji` struct value (see
 * {@link builtInSoundStructValue}): placing it in a `play sound` sound slot
 * bakes that constant into the program. The tiles are catalog-provided and
 * referenced by id, so they do not persist their struct value into a saved
 * brain.
 */
function registerBuiltInSoundTiles(api: WendooModuleApi): void {
  for (const def of BUILT_IN_SOUNDS) {
    api.registerTile(
      new BrainTileLiteralDef(
        SOUND_EMOJI_TYPE_ID,
        builtInSoundStructValue(def),
        { valueLabel: def.name, persist: false, metadata: { label: def.label, language: { form: def.label } } },
        api.brainServices
      )
    );
  }
}

function getDisplayReceiver(args: ReadonlyList<Value>): MicroBitDisplay | undefined {
  const receiver = args.at(0);
  if (!isStructNative(receiver, WODAL_MICROBIT_V2_TYPE_IDS.MicroBitDisplay)) {
    return undefined;
  }
  return receiver.native instanceof MicroBitDisplay ? receiver.native : undefined;
}

function getButtonReceiver(args: ReadonlyList<Value>): Button | TouchButton | undefined {
  const receiver = args.at(0);
  if (
    !isStructNative(receiver, WODAL_MICROBIT_V2_TYPE_IDS.Button) &&
    !isStructNative(receiver, WODAL_MICROBIT_V2_TYPE_IDS.TouchButton)
  ) {
    return undefined;
  }
  return receiver.native instanceof Button ? receiver.native : undefined;
}

function getTouchButtonReceiver(args: ReadonlyList<Value>): TouchButton | undefined {
  const receiver = args.at(0);
  if (!isStructNative(receiver, WODAL_MICROBIT_V2_TYPE_IDS.TouchButton)) {
    return undefined;
  }
  return receiver.native instanceof TouchButton ? receiver.native : undefined;
}

function getAccelerometerReceiver(args: ReadonlyList<Value>): Accelerometer | undefined {
  const receiver = args.at(0);
  if (!isStructNative(receiver, WODAL_MICROBIT_V2_TYPE_IDS.Accelerometer)) {
    return undefined;
  }
  return receiver.native instanceof Accelerometer ? receiver.native : undefined;
}

function getThermometerReceiver(args: ReadonlyList<Value>): Thermometer | undefined {
  const receiver = args.at(0);
  if (!isStructNative(receiver, WODAL_MICROBIT_V2_TYPE_IDS.Thermometer)) {
    return undefined;
  }
  return receiver.native instanceof Thermometer ? receiver.native : undefined;
}

function getI2CReceiver(args: ReadonlyList<Value>): I2CBus | undefined {
  const receiver = args.at(0);
  if (!isStructNative(receiver, WODAL_MICROBIT_V2_TYPE_IDS.I2C)) {
    return undefined;
  }
  return receiver.native instanceof I2CBus ? receiver.native : undefined;
}

function getGPIOReceiver(args: ReadonlyList<Value>): Gpio | undefined {
  const receiver = args.at(0);
  if (!isStructNative(receiver, WODAL_MICROBIT_V2_TYPE_IDS.GPIO)) {
    return undefined;
  }
  return receiver.native instanceof Gpio ? receiver.native : undefined;
}

function getSonarReceiver(args: ReadonlyList<Value>): SensorDriver | undefined {
  const receiver = args.at(0);
  if (!isStructNative(receiver, WODAL_MICROBIT_V2_TYPE_IDS.Sonar)) {
    return undefined;
  }
  return receiver.native instanceof SensorDriver ? receiver.native : undefined;
}

function getAudioReceiver(args: ReadonlyList<Value>): MicroBitSpeaker | undefined {
  const receiver = args.at(0);
  if (!isStructNative(receiver, WODAL_MICROBIT_V2_TYPE_IDS.MicroBitAudio)) {
    return undefined;
  }
  return receiver.native instanceof MicroBitSpeaker ? receiver.native : undefined;
}

function getRadioReceiver(args: ReadonlyList<Value>): Radio | undefined {
  const receiver = args.at(0);
  if (!isStructNative(receiver, WODAL_MICROBIT_V2_TYPE_IDS.Radio)) {
    return undefined;
  }
  return receiver.native instanceof Radio ? receiver.native : undefined;
}

/** The bytes of a `Buffer` argument, in order; an empty array when the value is not a buffer. */
function bufferArgBytes(value: Value | undefined): Uint8Array {
  if (!isBufferValue(value)) {
    return new Uint8Array(0);
  }
  const length = bufferLength(value);
  const bytes = new Uint8Array(length);
  for (let i = 0; i < length; i++) {
    bytes[i] = bufferByteAt(value, i) ?? 0;
  }
  return bytes;
}

function getNativeMicroBit(value: StructValue): MicroBit | undefined {
  return value.native instanceof MicroBit ? value.native : undefined;
}

function isStructNative(value: Value | undefined, typeId: string): value is StructValue {
  return value !== undefined && value.t === NativeType.Struct && value.typeId === typeId;
}

function numberArg(args: ReadonlyList<Value>, index: number): number {
  return extractNumberValue(args.at(index)) ?? 0;
}
