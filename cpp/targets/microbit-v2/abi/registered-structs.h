#pragma once

#include <array>
#include <cstdint>

#include "codal/shared-type-atom-id.h"
#include "core/runtime/type-registry.h"
#include "targets/microbit-v2/abi/type-atom-id.h"

namespace wendoo
{

/**
 * Field storage slot count of the `PlaySoundOptions` and `ScrollTextOptions`
 * value structs: their `immediately` and `inBackground` fields (ids 0, 1), so
 * highest field id + 1. Mirrors the LeaseOptionsField layout in
 * packages/wodal/src/targets/microbit-v2/wendoo/module.ts.
 */
inline constexpr uint32_t kLeaseOptionsSlotCount = 2;

/**
 * Field storage slot count of the `DrawImageOptions` value struct: its
 * `duration`, `immediately`, and `inBackground` fields (ids 0, 1, 2), so highest
 * field id + 1. Mirrors the DrawImageOptionsField layout in
 * packages/wodal/src/targets/microbit-v2/wendoo/module.ts.
 */
inline constexpr uint32_t kDrawImageOptionsSlotCount = 3;

/**
 * Field storage slot count of the `PlayToneOptions` value struct: its
 * `duration`, `volume`, `waveform`, `immediately`, and `inBackground` fields
 * (ids 0-4), so highest field id + 1. Mirrors the PlayToneOptionsField layout in
 * packages/wodal/src/targets/microbit-v2/wendoo/module.ts.
 */
inline constexpr uint32_t kPlayToneOptionsSlotCount = 5;

/**
 * Number of microbit-v2 registered (atom) value-struct slot-count entries: the
 * shared `Image` plus the four per-method device-API option structs.
 */
inline constexpr uint32_t kMicroBitV2RegisteredStructCount = kSharedRegisteredStructCount + 4;

/**
 * Builds the microbit-v2 registered (atom) value-struct slot-count table: every
 * struct a microbit-v2 program constructs at runtime via `STRUCT_NEW`, whose
 * atom TYPS entry carries no field shape. Covers the shared `Image` and the
 * device-API option structs (`PlaySoundOptions`, `DrawImageOptions`,
 * `ScrollTextOptions`, `PlayToneOptions`). Installed on a {@link TypeRegistry}
 * so `STRUCT_NEW` can size the allocation. The returned array must outlive any
 * registry it is installed into.
 */
inline std::array<RegisteredStructSlotCount, kMicroBitV2RegisteredStructCount>
makeMicroBitV2RegisteredStructSlotCounts()
{
    return {{
        {static_cast<uint32_t>(SharedTypeAtomId::Image), kSharedImageSlotCount},
        {static_cast<uint32_t>(MicroBitV2TypeAtomId::PlaySoundOptions), kLeaseOptionsSlotCount},
        {static_cast<uint32_t>(MicroBitV2TypeAtomId::DrawImageOptions), kDrawImageOptionsSlotCount},
        {static_cast<uint32_t>(MicroBitV2TypeAtomId::ScrollTextOptions), kLeaseOptionsSlotCount},
        {static_cast<uint32_t>(MicroBitV2TypeAtomId::PlayToneOptions), kPlayToneOptionsSlotCount},
    }};
}

} // namespace wendoo
