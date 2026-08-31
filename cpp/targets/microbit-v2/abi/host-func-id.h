#pragma once

#include <cstdint>

namespace wendoo
{

/**
 * Stable numeric funcIds for the microbit-v2 host functions: the native
 * struct methods and the sensor/actuator function entries. Mirrors the
 * MicroBitV2HostFuncId enum in
 * packages/wodal/src/targets/microbit-v2/wendoo/tile-ids.ts. `HOST_CALL`
 * dispatches by these values and serialized programs record them verbatim,
 * so the values are wire-stable: never renumber or reuse a value; append new
 * members at the next free id. All values are at or above core's
 * `TARGET_FUNC_ID_BASE`.
 */
enum class MicroBitV2HostFuncId : uint32_t
{
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
};

/**
 * Number of declared {@link MicroBitV2HostFuncId} members; ids are dense
 * from 1024.
 */
inline constexpr uint32_t kMicroBitV2HostFuncIdCount = 59;

} // namespace wendoo
