#pragma once

#include <cmath>
#include <cstdint>

#include "codal/device-port.h"
#include "core/runtime/mc-number.h"

namespace wendoo
{

/**
 * Pinned tone parameters of the microbit-v2 speaker port. Mirrors the tone
 * constants and `mkSpeakerToneCommand` in
 * packages/wodal/src/targets/microbit-v2/microbit-speaker.ts and the tone
 * defaults in wendoo/actions/play-tone.ts.
 */

/** Lowest tone pitch the speaker port accepts, in Hz. A 0 Hz tone is a silent rest. */
inline constexpr mc_number_t kMinToneFrequencyHz = 0.0f;

/** Highest tone pitch the speaker port accepts, in Hz. */
inline constexpr mc_number_t kMaxToneFrequencyHz = 9999.0f;

/** Pitch in Hz a tone sounds at when the call omits its frequency. */
inline constexpr mc_number_t kDefaultToneFrequencyHz = 880.0f;

/** Seconds a tone sounds for when the call omits its duration. */
inline constexpr mc_number_t kDefaultToneDurationSeconds = 0.5f;

/** Fraction of full tone volume a tone sounds at when the call omits its volume. */
inline constexpr mc_number_t kDefaultToneVolume = 1.0f;

/** Wave shape a tone sounds with when the call names none. */
inline constexpr SpeakerToneWaveform kDefaultToneWaveform = SpeakerToneWaveform::Triangle;

/** One wave shape's device-API name and the shape it selects. */
struct ToneWaveformDef
{
    const char *name;
    SpeakerToneWaveform waveform;
};

/** The wave shapes the speaker port sounds a tone with, by device-API name. */
inline constexpr ToneWaveformDef kToneWaveformTable[] = {
    {"square", SpeakerToneWaveform::Square},
    {"sawtooth", SpeakerToneWaveform::Sawtooth},
    {"sine", SpeakerToneWaveform::Sine},
    {"triangle", SpeakerToneWaveform::Triangle},
};

/**
 * Resolves the wave shape named by `length` ASCII bytes, writing it to
 * `waveform` and returning true. Returns false (leaving `waveform` untouched)
 * for a name outside {@link kToneWaveformTable}.
 */
inline bool findToneWaveform(const char *name, uint32_t length, SpeakerToneWaveform &waveform)
{
    for (const ToneWaveformDef &def : kToneWaveformTable)
    {
        uint32_t i = 0;
        while (i < length && def.name[i] != '\0' && def.name[i] == name[i])
        {
            i++;
        }
        if (i == length && def.name[i] == '\0')
        {
            waveform = def.waveform;
            return true;
        }
    }
    return false;
}

/**
 * The whole-millisecond play length of a tone whose duration argument is
 * `seconds`: scaled to milliseconds at the profile's f32 precision and rounded
 * half up, sign included, so a negative duration yields the negative length
 * that marks a dropped tone. A magnitude past the millisecond range saturates.
 */
inline int32_t toneDurationMs(mc_number_t seconds)
{
    const mc_number_t rounded = std::floor(seconds * 1000.0f + 0.5f);
    if (rounded >= 2147483648.0f)
    {
        return INT32_MAX;
    }
    if (rounded <= -2147483648.0f)
    {
        return INT32_MIN;
    }
    return static_cast<int32_t>(rounded);
}

/**
 * Builds the tone command the speaker port accepts, clamping the pitch into
 * {@link kMinToneFrequencyHz} to {@link kMaxToneFrequencyHz} and the volume
 * into 0 to 1. A 0 Hz tone is encoded as a silent rest: its volume is 0.
 * `durationMs` is carried through unchanged, sign included.
 */
inline SpeakerToneCommand mkSpeakerToneCommand(SpeakerToneWaveform waveform,
                                               mc_number_t frequencyHz, int32_t durationMs,
                                               mc_number_t volume)
{
    mc_number_t clampedFrequency = frequencyHz;
    if (clampedFrequency < kMinToneFrequencyHz)
    {
        clampedFrequency = kMinToneFrequencyHz;
    }
    else if (clampedFrequency > kMaxToneFrequencyHz)
    {
        clampedFrequency = kMaxToneFrequencyHz;
    }
    mc_number_t clampedVolume = volume;
    if (clampedVolume < 0.0f)
    {
        clampedVolume = 0.0f;
    }
    else if (clampedVolume > 1.0f)
    {
        clampedVolume = 1.0f;
    }
    return SpeakerToneCommand{waveform, clampedFrequency, durationMs,
                              clampedFrequency == kMinToneFrequencyHz ? 0.0f : clampedVolume};
}

} // namespace wendoo
