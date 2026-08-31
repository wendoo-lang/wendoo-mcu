#pragma once

#include <cstdint>

#include "core/runtime/core-host-actions.h"
#include "targets/microbit-v2/abi/host-func-id.h"

namespace wendoo
{

/**
 * Identity records of the microbit-v2 sensors and actuators, one per host
 * action. Mirrors the numeric ids of the MicroBitV2HostActions table in
 * packages/wodal/src/targets/microbit-v2/wendoo/tile-ids.ts; the records'
 * string keys are build-time identities and are not mirrored. Action ids are
 * at or above core's `TARGET_ACTION_ID_BASE` and are wire-stable: never
 * renumber or reuse one; append new records at the next free action id.
 */
namespace MicroBitV2HostActions
{
/** Sensor: button A, deriving one button event from the polled press level. */
inline constexpr HostActionIds ButtonA{1024,
                                       static_cast<uint32_t>(MicroBitV2HostFuncId::SensorButtonA)};

/** Actuator: set a single LED pixel brightness on the 5x5 display. */
inline constexpr HostActionIds DisplaySetPixel{
    1025, static_cast<uint32_t>(MicroBitV2HostFuncId::ActuatorDisplaySetPixel)};

/**
 * Actuator: show text on the 5x5 display (scrolled, or static for one
 * character), awaiting the animation.
 */
inline constexpr HostActionIds DisplayScroll{
    1026, static_cast<uint32_t>(MicroBitV2HostFuncId::ActuatorDisplayScroll)};

/** Sensor: button B, deriving one button event from the polled press level. */
inline constexpr HostActionIds ButtonB{1027,
                                       static_cast<uint32_t>(MicroBitV2HostFuncId::SensorButtonB)};

/** Sensor: buttons A and B together, pressed only while both are pressed. */
inline constexpr HostActionIds ButtonAB{
    1028, static_cast<uint32_t>(MicroBitV2HostFuncId::SensorButtonAB)};

/** Sensor: the capacitive touch logo, deriving events from the polled touch level. */
inline constexpr HostActionIds ButtonLogo{
    1029, static_cast<uint32_t>(MicroBitV2HostFuncId::SensorButtonLogo)};

/** Sensor: an accelerometer gesture, true while the polled gesture matches the chosen modifier. */
inline constexpr HostActionIds Gesture{1030,
                                       static_cast<uint32_t>(MicroBitV2HostFuncId::SensorGesture)};

/** Actuator: paste an image to the 5x5 display, optionally holding it for a duration. */
inline constexpr HostActionIds DrawImage{
    1031, static_cast<uint32_t>(MicroBitV2HostFuncId::ActuatorDrawImage)};

/** Actuator: broadcast the optional value (or the WHEN-result) as a radio packet. */
inline constexpr HostActionIds RadioSend{
    1032, static_cast<uint32_t>(MicroBitV2HostFuncId::ActuatorRadioSend)};

/** Sensor: the next received NUMBER / DOUBLE packet, delivering its numeric value. */
inline constexpr HostActionIds RadioReceiveNumber{
    1033, static_cast<uint32_t>(MicroBitV2HostFuncId::SensorRadioReceiveNumber)};

/** Sensor: the next received STRING packet, delivering its string value. */
inline constexpr HostActionIds RadioReceiveString{
    1034, static_cast<uint32_t>(MicroBitV2HostFuncId::SensorRadioReceiveString)};

/** Actuator: set the radio group (0-255). */
inline constexpr HostActionIds SetRadioGroup{
    1035, static_cast<uint32_t>(MicroBitV2HostFuncId::ActuatorSetRadioGroup)};

/** Sensor: the next received BUFFER packet, delivering its raw payload Buffer. */
inline constexpr HostActionIds RadioReceiveBuffer{
    1036, static_cast<uint32_t>(MicroBitV2HostFuncId::SensorRadioReceiveBuffer)};

/** Actuator: play a built-in sound on the speaker, awaiting its nominal duration. */
inline constexpr HostActionIds PlaySound{
    1037, static_cast<uint32_t>(MicroBitV2HostFuncId::ActuatorPlaySound)};

/** Actuator: blank the 5x5 display, cancelling any held display lease. */
inline constexpr HostActionIds DisplayClear{
    1038, static_cast<uint32_t>(MicroBitV2HostFuncId::ActuatorDisplayClear)};

/** Sensor: the ambient light level read off the LED matrix, 0 (dark) to 255 (bright). */
inline constexpr HostActionIds LightLevel{
    1039, static_cast<uint32_t>(MicroBitV2HostFuncId::SensorLightLevel)};

/** Sensor: the die temperature in whole degrees Celsius; signed. */
inline constexpr HostActionIds Temperature{
    1040, static_cast<uint32_t>(MicroBitV2HostFuncId::SensorTemperature)};

/** Actuator: sound a plain constant-pitch tone on the speaker, awaiting its duration. */
inline constexpr HostActionIds PlayTone{
    1041, static_cast<uint32_t>(MicroBitV2HostFuncId::ActuatorPlayTone)};
} // namespace MicroBitV2HostActions

/**
 * All microbit-v2 host-action records, in action-id order; ids are dense
 * from 1024.
 */
inline constexpr HostActionIds kMicroBitV2HostActions[] = {
    MicroBitV2HostActions::ButtonA,
    MicroBitV2HostActions::DisplaySetPixel,
    MicroBitV2HostActions::DisplayScroll,
    MicroBitV2HostActions::ButtonB,
    MicroBitV2HostActions::ButtonAB,
    MicroBitV2HostActions::ButtonLogo,
    MicroBitV2HostActions::Gesture,
    MicroBitV2HostActions::DrawImage,
    MicroBitV2HostActions::RadioSend,
    MicroBitV2HostActions::RadioReceiveNumber,
    MicroBitV2HostActions::RadioReceiveString,
    MicroBitV2HostActions::SetRadioGroup,
    MicroBitV2HostActions::RadioReceiveBuffer,
    MicroBitV2HostActions::PlaySound,
    MicroBitV2HostActions::DisplayClear,
    MicroBitV2HostActions::LightLevel,
    MicroBitV2HostActions::Temperature,
    MicroBitV2HostActions::PlayTone,
};

} // namespace wendoo
