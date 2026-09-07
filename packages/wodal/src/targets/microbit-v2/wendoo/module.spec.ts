import assert from "node:assert/strict";
import { test } from "node:test";
import {
  type AsyncHandle,
  type BrainTileLiteralDef,
  ContextTypeIds,
  type ExecutionContext,
  extractNumberValue,
  isStructValue,
  List,
  mkNativeStructValue,
  mkNumberValue,
  mkStringValue,
  NativeType,
  NIL_VALUE,
  type StructTypeDef,
  type Value,
  type WendooEnvironment,
} from "@wendoo/core/app";
import { bufferToHex, isBufferValue, mkBufferValueFromHex } from "@wendoo/core/runtime";
import { ImageField, WODAL_SHARED_TYPE_IDS } from "../../../wendoo/shared-type-ids";
import { MicroBit } from "../microbit";
import { BUILT_IN_IMAGES, builtInImageHex, builtInImageTileId } from "./built-in-images";
import { createMicroBitV2Environment } from "./environment";
import { CONTEXT_MICROBIT_FIELD_ID, MicroBitField, WODAL_MICROBIT_V2_TYPE_IDS } from "./module";

test("Context.microbit exposes the native WODAL microbit device", () => {
  const env = createMicroBitV2Environment();
  const microbit = new MicroBit();
  const ctx = createExecutionContext(env, microbit);
  const contextDef = getStructType(env, ContextTypeIds.Context);
  const microbitDef = getStructType(env, WODAL_MICROBIT_V2_TYPE_IDS.MicroBit);

  const microbitValue = contextDef.fieldGetter?.(
    mkNativeStructValue(ContextTypeIds.Context, ctx),
    CONTEXT_MICROBIT_FIELD_ID,
    ctx
  );
  assert.ok(isStructValue(microbitValue));
  assert.equal(microbitValue.typeId, WODAL_MICROBIT_V2_TYPE_IDS.MicroBit);
  assert.equal(microbitValue.native, microbit);

  const displayValue = microbitDef.fieldGetter?.(microbitValue, MicroBitField.Display, ctx);
  assert.ok(isStructValue(displayValue));
  assert.equal(displayValue.typeId, WODAL_MICROBIT_V2_TYPE_IDS.MicroBitDisplay);
  assert.equal(displayValue.native, microbit.display);

  const buttonAValue = microbitDef.fieldGetter?.(microbitValue, MicroBitField.ButtonA, ctx);
  assert.ok(isStructValue(buttonAValue));
  assert.equal(buttonAValue.typeId, WODAL_MICROBIT_V2_TYPE_IDS.Button);
  assert.equal(buttonAValue.native, microbit.buttonA);

  const logoValue = microbitDef.fieldGetter?.(microbitValue, MicroBitField.Logo, ctx);
  assert.ok(isStructValue(logoValue));
  assert.equal(logoValue.typeId, WODAL_MICROBIT_V2_TYPE_IDS.TouchButton);
  assert.equal(logoValue.native, microbit.logo);

  const i2cValue = microbitDef.fieldGetter?.(microbitValue, MicroBitField.I2C, ctx);
  assert.ok(isStructValue(i2cValue));
  assert.equal(i2cValue.typeId, WODAL_MICROBIT_V2_TYPE_IDS.I2C);
  assert.equal(i2cValue.native, microbit.i2c);

  const gpioValue = microbitDef.fieldGetter?.(microbitValue, MicroBitField.GPIO, ctx);
  assert.ok(isStructValue(gpioValue));
  assert.equal(gpioValue.typeId, WODAL_MICROBIT_V2_TYPE_IDS.GPIO);
  assert.equal(gpioValue.native, microbit.gpio);

  const thermometerValue = microbitDef.fieldGetter?.(microbitValue, MicroBitField.Thermometer, ctx);
  assert.ok(isStructValue(thermometerValue));
  assert.equal(thermometerValue.typeId, WODAL_MICROBIT_V2_TYPE_IDS.Thermometer);
  assert.equal(thermometerValue.native, microbit.thermometer);

  // The audio receiver is the device's one speaker: the Device API drives the
  // same MicroBitSpeaker instance the play-sound tile drives.
  const audioValue = microbitDef.fieldGetter?.(microbitValue, MicroBitField.Audio, ctx);
  assert.ok(isStructValue(audioValue));
  assert.equal(audioValue.typeId, WODAL_MICROBIT_V2_TYPE_IDS.MicroBitAudio);
  assert.equal(audioValue.native, microbit.speaker);
});

test("MicroBitAudio.playSound routes through the native speaker receiver", () => {
  const env = createMicroBitV2Environment();
  const microbit = new MicroBit();
  const ctx = createExecutionContext(env, microbit);
  const receiver = mkNativeStructValue(WODAL_MICROBIT_V2_TYPE_IDS.MicroBitAudio, microbit.speaker);
  const playSound = getAsyncFunction(env, "MicroBitAudio.playSound");

  // An accepted built-in play takes the speaker lease and parks its handle
  // until the nominal duration elapses.
  let resolved = 0;
  const handle: AsyncHandle = { id: 1, resolve: () => resolved++, reject: () => {}, cancel: () => {} };
  playSound.exec(ctx, List.from<Value>([receiver, mkStringValue("happy")]), handle);
  assert.equal(microbit.speaker.isBusy(), true);
  assert.equal(microbit.speaker.snapshot().playing?.name, "happy");
  assert.equal(resolved, 0);
  microbit.speaker.advancePlay(1259);
  assert.equal(resolved, 1);
  assert.equal(microbit.speaker.isBusy(), false);

  // A name outside the built-in set is a silent no-op: no lease, resolved at once.
  let noopResolved = 0;
  const noopHandle: AsyncHandle = { id: 2, resolve: () => noopResolved++, reject: () => {}, cancel: () => {} };
  playSound.exec(ctx, List.from<Value>([receiver, mkStringValue("bogus")]), noopHandle);
  assert.equal(microbit.speaker.isBusy(), false);
  assert.equal(noopResolved, 1);
});

test("I2C.writeBuffer routes the address and bytes through the native bus receiver", () => {
  const env = createMicroBitV2Environment();
  const microbit = new MicroBit();
  const ctx = createExecutionContext(env, microbit);
  const receiver = mkNativeStructValue(WODAL_MICROBIT_V2_TYPE_IDS.I2C, microbit.i2c);

  const status = getSyncFunction(env, "I2C.writeBuffer").exec(
    ctx,
    List.from([receiver, mkNumberValue(0x10), mkBufferValueFromHex("0102030405")])
  );

  assert.equal(extractNumberValue(status), 0);
  const writes = microbit.i2c.recordedWrites();
  assert.equal(writes.length, 1);
  assert.equal(writes[0]?.address, 0x10);
  assert.deepEqual(Array.from(writes[0]?.bytes ?? []), [1, 2, 3, 4, 5]);
});

test("I2C.readBuffer returns the injected response bytes for the address as a Buffer", () => {
  const env = createMicroBitV2Environment();
  const microbit = new MicroBit();
  const ctx = createExecutionContext(env, microbit);
  const receiver = mkNativeStructValue(WODAL_MICROBIT_V2_TYPE_IDS.I2C, microbit.i2c);
  microbit.i2c.setReadResponse(0x42, Uint8Array.from([0xaa, 0xbb, 0xcc]));

  const data = getSyncFunction(env, "I2C.readBuffer").exec(
    ctx,
    List.from([receiver, mkNumberValue(0x42), mkNumberValue(3)])
  );
  assert.ok(isBufferValue(data));
  assert.equal(bufferToHex(data), "aabbcc");

  // An address with no injected response is a no-device read: an empty Buffer.
  const empty = getSyncFunction(env, "I2C.readBuffer").exec(
    ctx,
    List.from([receiver, mkNumberValue(0x55), mkNumberValue(2)])
  );
  assert.ok(isBufferValue(empty));
  assert.equal(bufferToHex(empty), "");
});

test("GPIO host methods route through the native pin receiver", () => {
  const env = createMicroBitV2Environment();
  const microbit = new MicroBit();
  const ctx = createExecutionContext(env, microbit);
  const receiver = mkNativeStructValue(WODAL_MICROBIT_V2_TYPE_IDS.GPIO, microbit.gpio);
  microbit.gpio.setDigitalRead(13, 1);
  microbit.gpio.setAnalogRead(1, 1023);

  const level = getSyncFunction(env, "GPIO.digitalRead").exec(ctx, List.from([receiver, mkNumberValue(13)]));
  assert.equal(extractNumberValue(level), 1);

  const analog = getSyncFunction(env, "GPIO.analogRead").exec(ctx, List.from([receiver, mkNumberValue(1)]));
  assert.equal(extractNumberValue(analog), 1023);

  // An uninjected pin reads 0.
  const idle = getSyncFunction(env, "GPIO.analogRead").exec(ctx, List.from([receiver, mkNumberValue(2)]));
  assert.equal(extractNumberValue(idle), 0);

  const writeStatus = getSyncFunction(env, "GPIO.digitalWrite").exec(
    ctx,
    List.from([receiver, mkNumberValue(2), mkNumberValue(1)])
  );
  assert.equal(extractNumberValue(writeStatus), 0);

  const pullStatus = getSyncFunction(env, "GPIO.setPull").exec(
    ctx,
    List.from([receiver, mkNumberValue(13), mkNumberValue(0)])
  );
  assert.equal(extractNumberValue(pullStatus), 0);

  const servoStatus = getSyncFunction(env, "GPIO.servoWrite").exec(
    ctx,
    List.from([receiver, mkNumberValue(1), mkNumberValue(90)])
  );
  assert.equal(extractNumberValue(servoStatus), 0);

  assert.deepEqual(
    microbit.gpio.recordedDigitalWrites().map((w) => ({ pin: w.pin, value: w.value })),
    [{ pin: 2, value: 1 }]
  );
  assert.deepEqual(
    microbit.gpio.recordedPulls().map((p) => ({ pin: p.pin, mode: p.mode })),
    [{ pin: 13, mode: 0 }]
  );
  assert.deepEqual(
    microbit.gpio.recordedServos().map((s) => ({ pin: s.pin, angle: s.angle })),
    [{ pin: 1, angle: 90 }]
  );

  // A pin outside 0-20 is a no-op: nothing recorded, a benign 0 returned.
  const oob = getSyncFunction(env, "GPIO.digitalWrite").exec(
    ctx,
    List.from([receiver, mkNumberValue(99), mkNumberValue(1)])
  );
  assert.equal(extractNumberValue(oob), 0);
  assert.equal(microbit.gpio.recordedDigitalWrites().length, 1);
});

test("MicroBitDisplay host methods route through the native display receiver", () => {
  const env = createMicroBitV2Environment();
  const microbit = new MicroBit();
  const ctx = createExecutionContext(env, microbit);
  const receiver = mkNativeStructValue(WODAL_MICROBIT_V2_TYPE_IDS.MicroBitDisplay, microbit.display);

  // 300 narrows to CODAL's uint8 brightness by wrapping (300 & 0xff = 44).
  getSyncFunction(env, "MicroBitDisplay.setPixelValue").exec(
    ctx,
    List.from([receiver, mkNumberValue(1), mkNumberValue(2), mkNumberValue(300)])
  );

  assert.equal(microbit.display.getPixelValue(1, 2), 44);

  const brightness = getSyncFunction(env, "MicroBitDisplay.getPixelValue").exec(
    ctx,
    List.from([receiver, mkNumberValue(1), mkNumberValue(2)])
  );
  assert.equal(extractNumberValue(brightness), 44);

  getSyncFunction(env, "MicroBitDisplay.clear").exec(ctx, List.from([receiver]));

  assert.equal(microbit.display.getPixelValue(1, 2), 0);
});

test("microbit display host state is scoped to each execution context", () => {
  const firstEnv = createMicroBitV2Environment();
  const secondEnv = createMicroBitV2Environment();
  const firstMicroBit = new MicroBit();
  const secondMicroBit = new MicroBit();
  const firstReceiver = mkNativeStructValue(WODAL_MICROBIT_V2_TYPE_IDS.MicroBitDisplay, firstMicroBit.display);
  const secondReceiver = mkNativeStructValue(WODAL_MICROBIT_V2_TYPE_IDS.MicroBitDisplay, secondMicroBit.display);

  getSyncFunction(firstEnv, "MicroBitDisplay.setPixelValue").exec(
    createExecutionContext(firstEnv, firstMicroBit),
    List.from([firstReceiver, mkNumberValue(0), mkNumberValue(0), mkNumberValue(31)])
  );
  getSyncFunction(secondEnv, "MicroBitDisplay.setPixelValue").exec(
    createExecutionContext(secondEnv, secondMicroBit),
    List.from([secondReceiver, mkNumberValue(0), mkNumberValue(0), mkNumberValue(127)])
  );

  assert.equal(firstMicroBit.display.getPixelValue(0, 0), 31);
  assert.equal(secondMicroBit.display.getPixelValue(0, 0), 127);
});

test("Button host methods route through native button receivers", () => {
  const env = createMicroBitV2Environment();
  const microbit = new MicroBit();
  const ctx = createExecutionContext(env, microbit);
  const buttonA = mkNativeStructValue(WODAL_MICROBIT_V2_TYPE_IDS.Button, microbit.buttonA);
  const buttonB = mkNativeStructValue(WODAL_MICROBIT_V2_TYPE_IDS.Button, microbit.buttonB);
  const logo = mkNativeStructValue(WODAL_MICROBIT_V2_TYPE_IDS.TouchButton, microbit.logo);

  microbit.setButtonPressed("A", true);
  assert.equal(extractNumberValue(getSyncFunction(env, "Button.isPressed").exec(ctx, List.from([buttonA]))), 1);
  assert.equal(extractNumberValue(getSyncFunction(env, "Button.isPressed").exec(ctx, List.from([buttonB]))), 0);

  microbit.setButtonPressed("B", true);
  assert.equal(extractNumberValue(getSyncFunction(env, "Button.isPressed").exec(ctx, List.from([buttonB]))), 1);

  getSyncFunction(env, "TouchButton.setThreshold").exec(ctx, List.from([logo, mkNumberValue(5)]));
  getSyncFunction(env, "TouchButton.setValue").exec(ctx, List.from([logo, mkNumberValue(5)]));

  assert.equal(extractNumberValue(getSyncFunction(env, "TouchButton.isPressed").exec(ctx, List.from([logo]))), 1);
  assert.equal(extractNumberValue(getSyncFunction(env, "TouchButton.getThreshold").exec(ctx, List.from([logo]))), 5);
  assert.equal(extractNumberValue(getSyncFunction(env, "TouchButton.getValue").exec(ctx, List.from([logo]))), 5);
});

test("each built-in image registers a literal tile carrying its baked Image struct", () => {
  const env = createMicroBitV2Environment();
  const tiles = env.brainServices.edit.tiles;
  for (const def of BUILT_IN_IMAGES) {
    const tile = tiles.get(builtInImageTileId(def));
    assert.ok(tile, `built-in image tile '${def.name}' should be registered`);
    assert.equal(tile.kind, "literal");
    assert.equal(tile.persist, false);
    assert.equal((tile as BrainTileLiteralDef).uniqueId, undefined);
    const value = (tile as BrainTileLiteralDef).value as Value | undefined;
    assert.ok(isStructValue(value));
    assert.equal(value.typeId, WODAL_SHARED_TYPE_IDS.Image);
    const pixels = value.v?.at(ImageField.Pixels);
    assert.ok(isBufferValue(pixels));
    assert.equal(bufferToHex(pixels), builtInImageHex(def));
  }
});

function createExecutionContext(env: WendooEnvironment, microbit: MicroBit): ExecutionContext {
  return {
    services: env.brainServices as unknown as ExecutionContext["services"],
    getVariableBySlot: () => NIL_VALUE,
    setVariableBySlot: () => {},
    getSystemVarBySlot: () => NIL_VALUE,
    setSystemVarBySlot: () => {},
    data: { microbit },
    time: 0,
    dt: 0,
    currentTick: 0,
  };
}

function getStructType(env: WendooEnvironment, typeId: string): StructTypeDef {
  const typeDef = env.brainServices.runtime.types.get(typeId);
  assert.ok(typeDef);
  assert.equal(typeDef.coreType, NativeType.Struct);
  return typeDef as StructTypeDef;
}

function getSyncFunction(env: WendooEnvironment, name: string) {
  const entry = env.brainServices.runtime.functions.get(name);
  assert.ok(entry);
  assert.equal(entry.isAsync, false);
  return entry.fn;
}

function getAsyncFunction(env: WendooEnvironment, name: string) {
  const entry = env.brainServices.runtime.functions.get(name);
  assert.ok(entry);
  assert.equal(entry.isAsync, true);
  return entry.fn;
}
