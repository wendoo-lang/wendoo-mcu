#pragma once

#include <cstdint>

#include "core/platform/span.h"
#include "core/runtime/mc-number.h"
#include "core/runtime/value.h"

namespace wendoo
{

namespace detail
{

/** The slot's value when present and non-nil, mirroring the TS `hasArg`. */
inline bool hasArg(Span<const Value> args, uint32_t slotId)
{
    return slotId < args.size() && !args[slotId].isNil();
}

/** The slot's numeric payload, or `fallback` when absent or non-numeric. */
inline mc_number_t numberArgOr(Span<const Value> args, uint32_t slotId, mc_number_t fallback)
{
    if (slotId < args.size() && args[slotId].isNumber())
    {
        return args[slotId].asNumber();
    }
    return fallback;
}

/** The number `value` holds, or `fallback` when it is not a number or is non-finite. */
inline mc_number_t finiteNumberOr(const Value &value, mc_number_t fallback)
{
    if (!value.isNumber())
    {
        return fallback;
    }
    const mc_number_t number = value.asNumber();
    if (number != number || number - number != 0.0f) // NaN or infinity
    {
        return fallback;
    }
    return number;
}

/** The slot's numeric payload, or `fallback` when absent, non-numeric, or non-finite. */
inline mc_number_t finiteNumberArgOr(Span<const Value> args, uint32_t slotId, mc_number_t fallback)
{
    return finiteNumberOr(slotId < args.size() ? args[slotId] : kNilValue, fallback);
}

/**
 * Converts a pixel coordinate to the display port's int16 parameter: non-finite
 * values become 0; finite values truncate toward zero and narrow to int16. The
 * device applies the matrix-bounds check (CODAL `Image::setPixelValue` drops a
 * coordinate outside 0..dimension).
 */
inline int16_t pixelCoordToPort(mc_number_t value)
{
    if (value != value || value - value != 0.0f) // NaN or infinity
    {
        return 0;
    }
    return static_cast<int16_t>(static_cast<int32_t>(value));
}

/**
 * Converts a brightness to the port's uint8 parameter: non-finite values become
 * 0; finite values truncate toward zero and narrow to uint8 (CODAL
 * `Image::setPixelValue` stores the uint8 value with no clamp).
 */
inline uint8_t brightnessToPort(mc_number_t value)
{
    if (value != value || value - value != 0.0f)
    {
        return 0;
    }
    return static_cast<uint8_t>(static_cast<int32_t>(value));
}

} // namespace detail

} // namespace wendoo
