#pragma once

#include <cstdint>

namespace wendoo
{

/**
 * Stable type-atom ids of the microbit-v2 native struct types. Mirrors the
 * MicroBitV2TypeAtomId enum in
 * packages/wodal/src/targets/microbit-v2/wendoo/tile-ids.ts. Serialized
 * programs reference nominal types by these values, so the values are
 * wire-stable: never renumber or reuse a value; append new members at the
 * next free id. All values are at or above core's `TARGET_TYPE_ATOM_BASE`.
 */
enum class MicroBitV2TypeAtomId : uint32_t
{
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
};

/**
 * Number of declared {@link MicroBitV2TypeAtomId} members; ids are dense
 * from 1024.
 */
inline constexpr uint32_t kMicroBitV2TypeAtomIdCount = 18;

} // namespace wendoo
