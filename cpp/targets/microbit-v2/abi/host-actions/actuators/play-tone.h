#pragma once

#include <cstdint>

#include "codal/device-port.h"
#include "core/platform/span.h"
#include "core/runtime/execution-context.h"
#include "core/runtime/handle-table.h"
#include "core/runtime/result.h"
#include "core/runtime/value.h"
#include "targets/microbit-v2/abi/host-binding-conversions.h"
#include "targets/microbit-v2/abi/speaker-tone.h"

namespace wendoo
{

/**
 * Positional arg slots of the play-tone actuator, in the flattened
 * call-definition order of the wodal action source
 * (packages/wodal/src/targets/microbit-v2/wendoo/actions/play-tone.ts): the
 * anonymous pitch, the named duration and volume, the four wave-shape
 * modifiers, then the lease modifier pair. Slot order is wire-stable with the
 * compiler's emitted arg buffers.
 */
inline constexpr uint32_t kPlayToneFrequencyArgSlot = 0;
inline constexpr uint32_t kPlayToneDurationArgSlot = 1;
inline constexpr uint32_t kPlayToneVolumeArgSlot = 2;
inline constexpr uint32_t kPlayToneSquareArgSlot = 3;
inline constexpr uint32_t kPlayToneSawtoothArgSlot = 4;
inline constexpr uint32_t kPlayToneSineArgSlot = 5;
inline constexpr uint32_t kPlayToneTriangleArgSlot = 6;

/** Arg slot of the `immediately` modifier: when present, the tone preempts the current lease. */
inline constexpr uint32_t kPlayToneImmediatelyArgSlot = 7;

/**
 * Arg slot of the `in background` modifier: when present, the tone keeps its
 * lease but the handle resolves at dispatch so the issuing rule does not await it.
 */
inline constexpr uint32_t kPlayToneInBackgroundArgSlot = 8;

namespace detail
{

/**
 * The wave shape the attached modifier selects; {@link kDefaultToneWaveform}
 * when none is attached. The modifiers form one choice group, so at most one
 * slot is present.
 */
inline SpeakerToneWaveform selectToneWaveform(Span<const Value> args)
{
    if (hasArg(args, kPlayToneSquareArgSlot))
    {
        return SpeakerToneWaveform::Square;
    }
    if (hasArg(args, kPlayToneSawtoothArgSlot))
    {
        return SpeakerToneWaveform::Sawtooth;
    }
    if (hasArg(args, kPlayToneSineArgSlot))
    {
        return SpeakerToneWaveform::Sine;
    }
    if (hasArg(args, kPlayToneTriangleArgSlot))
    {
        return SpeakerToneWaveform::Triangle;
    }
    return kDefaultToneWaveform;
}

} // namespace detail

/**
 * Async host actuator body: sound a plain constant-pitch tone on the speaker.
 * Reads the optional anonymous pitch in Hz (default 880, clamped into 0-9999,
 * where 0 is a silent rest that still holds the speaker), the named duration in
 * seconds (default 0.5), the named volume as a 0-1 fraction of full (default 1,
 * clamped), and at most one wave-shape modifier (`square`, `sawtooth`, `sine`,
 * or `triangle`; triangle when none is attached). An absent, nil, or non-finite
 * number reads as its default. With the `immediately` modifier present it
 * preempts the current speaker lease at dispatch, before the new tone is
 * examined. Starts the tone on the speaker port at the current think time and
 * leaves `handle` for the port to settle: an accepted tone resolves once its
 * duration elapses, and a tone the busy speaker drops or one with a negative
 * duration resolves at once. With the `in background` modifier present the tone
 * keeps its lease but `handle` resolves at dispatch, so the issuing rule
 * continues this round without parking on it. `hostData` is the bound
 * {@link DevicePorts}. Mirrors wodal `actions/play-tone.ts`.
 */
inline Status execPlayTone(void *hostData, ExecutionContext &ctx, Span<const Value> args,
                           AsyncHandle handle)
{
    DevicePorts &ports = *static_cast<DevicePorts *>(hostData);
    if (detail::hasArg(args, kPlayToneImmediatelyArgSlot))
    {
        ports.speaker->preempt();
    }
    const SpeakerToneCommand command = mkSpeakerToneCommand(
        detail::selectToneWaveform(args),
        detail::finiteNumberArgOr(args, kPlayToneFrequencyArgSlot, kDefaultToneFrequencyHz),
        toneDurationMs(
            detail::finiteNumberArgOr(args, kPlayToneDurationArgSlot, kDefaultToneDurationSeconds)),
        detail::finiteNumberArgOr(args, kPlayToneVolumeArgSlot, kDefaultToneVolume));
    ports.speaker->playTone(command, ctx.time, handle);
    if (detail::hasArg(args, kPlayToneInBackgroundArgSlot))
    {
        // The tone keeps its lease and resolves on tick time as above; resolving
        // now releases the issuing rule so it does not park.
        handle.resolve(kVoidValue);
    }
    return Status::ok();
}

} // namespace wendoo
