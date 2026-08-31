#pragma once

#include <cstdint>

#include "codal/device-port.h"
#include "core/platform/span.h"
#include "core/runtime/execution-context.h"
#include "core/runtime/handle-table.h"
#include "core/runtime/managed-heap.h"
#include "core/runtime/result.h"
#include "core/runtime/value.h"
#include "targets/microbit-v2/abi/host-binding-conversions.h"
#include "targets/microbit-v2/abi/host-functions/native-receiver.h"
#include "targets/microbit-v2/abi/speaker-tone.h"
#include "targets/microbit-v2/abi/type-atom-id.h"

namespace wendoo
{

/**
 * Positional arg slot of the `MicroBitAudio.playTone` host function: arg 0 is
 * the audio receiver, arg 1 the pitch in Hz.
 */
inline constexpr uint32_t kAudioPlayToneHostFnFrequencyArgSlot = 1;

/**
 * Arg slot of the optional `PlayToneOptions` struct, and its field ids: the
 * three tone controls ahead of the two lease flags. `duration` is in seconds
 * and `volume` a 0-1 fraction; `waveform` names a wave shape
 * ({@link kToneWaveformTable}); `immediately` preempts the current lease at
 * dispatch and `inBackground` resolves the handle at dispatch so the caller
 * does not await the tone. Each field reads as its default when the struct or
 * the field is absent.
 */
inline constexpr uint32_t kAudioPlayToneHostFnOptionsArgSlot = 2;
inline constexpr uint32_t kPlayToneOptionsDurationField = 0;
inline constexpr uint32_t kPlayToneOptionsVolumeField = 1;
inline constexpr uint32_t kPlayToneOptionsWaveformField = 2;
inline constexpr uint32_t kPlayToneOptionsImmediatelyField = 3;
inline constexpr uint32_t kPlayToneOptionsInBackgroundField = 4;

/**
 * The speaker port and managed heap an async play-tone host-function body
 * reaches: the speaker to start the tone, the heap to read the options struct
 * and its `waveform` name string. The caller fills both before the binding's
 * first dispatch.
 */
struct MicroBitV2PlayToneEnv
{
    SpeakerPort *speaker;
    ManagedHeap *heap;
};

/**
 * Async host function `MicroBitAudio.playTone`: sound a plain constant-pitch
 * tone on the speaker. Arg 0 is the audio receiver, arg 1 the pitch in Hz
 * (default 880, clamped into 0-9999, where 0 is a silent rest that still holds
 * the speaker), arg 2 the optional `PlayToneOptions` struct carrying the
 * duration in seconds (default 0.5), the 0-1 volume fraction (default 1,
 * clamped), the wave-shape name (default triangle), and the two lease flags. An
 * absent, nil, or non-finite number reads as its default. When `immediately` is
 * true the current speaker lease is preempted at dispatch, before the tone is
 * examined, so a waveform name outside the sounded set still preempts; such a
 * name then sounds nothing and resolves the handle at once.
 * Starts the tone on the speaker port at the current think time and leaves
 * `handle` for the port to settle: an accepted tone resolves once its duration
 * elapses, and a tone the busy speaker drops or one with a negative duration
 * resolves at once. When `inBackground` is true the tone keeps its lease but
 * `handle` resolves at dispatch, so the caller continues this round without
 * parking on it. An unrecognized receiver resolves the handle at once.
 * `hostData` is the bound {@link MicroBitV2PlayToneEnv}. Mirrors the
 * `MicroBitAudio.playTone` host function in
 * packages/wodal/src/targets/microbit-v2/wendoo/module.ts.
 */
inline Status execAudioPlayToneHostFn(void *hostData, ExecutionContext &ctx, Span<const Value> args,
                                      AsyncHandle handle)
{
    MicroBitV2PlayToneEnv &env = *static_cast<MicroBitV2PlayToneEnv *>(hostData);
    if (args.empty() || !detail::isReceiver(args[0], MicroBitV2TypeAtomId::MicroBitAudio))
    {
        handle.resolve(kVoidValue);
        return Status::ok();
    }
    if (optionStructFlag(*env.heap, args, kAudioPlayToneHostFnOptionsArgSlot,
                         kPlayToneOptionsImmediatelyField))
    {
        env.speaker->preempt();
    }
    SpeakerToneWaveform waveform = kDefaultToneWaveform;
    const Value waveformField = optionStructField(
        *env.heap, args, kAudioPlayToneHostFnOptionsArgSlot, kPlayToneOptionsWaveformField);
    if (!waveformField.isNil())
    {
        const char *bytes = nullptr;
        uint32_t length = 0;
        if (!env.heap->stringContent(waveformField, bytes, length) ||
            !findToneWaveform(bytes, length, waveform))
        {
            handle.resolve(kVoidValue);
            return Status::ok();
        }
    }
    const mc_number_t durationSeconds = detail::finiteNumberOr(
        optionStructField(*env.heap, args, kAudioPlayToneHostFnOptionsArgSlot,
                          kPlayToneOptionsDurationField),
        kDefaultToneDurationSeconds);
    const SpeakerToneCommand command = mkSpeakerToneCommand(
        waveform,
        detail::finiteNumberArgOr(args, kAudioPlayToneHostFnFrequencyArgSlot,
                                  kDefaultToneFrequencyHz),
        toneDurationMs(durationSeconds),
        detail::finiteNumberOr(optionStructField(*env.heap, args,
                                                 kAudioPlayToneHostFnOptionsArgSlot,
                                                 kPlayToneOptionsVolumeField),
                               kDefaultToneVolume));
    env.speaker->playTone(command, ctx.time, handle);
    if (optionStructFlag(*env.heap, args, kAudioPlayToneHostFnOptionsArgSlot,
                         kPlayToneOptionsInBackgroundField))
    {
        // The tone keeps its lease and resolves on tick time as above; resolving
        // now releases the caller so it does not park.
        handle.resolve(kVoidValue);
    }
    return Status::ok();
}

} // namespace wendoo
