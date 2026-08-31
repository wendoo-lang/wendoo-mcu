import type { SpeakerToneCommand } from "./microbit-speaker";
import { decodeBuiltInSoundRaw, type RawSoundEffect, type RawToneEffect, type SoundWaveform } from "./sound-expression";
import { BUILT_IN_SOUNDS } from "./wendoo/built-in-sounds";

/**
 * Sample rate of the rendered PCM, in Hz. Matches CODAL's
 * `EMOJI_SYNTHESIZER_SAMPLE_RATE`. A Web Audio consumer builds an `AudioBuffer`
 * at this rate; the browser resamples to the output device rate, preserving
 * pitch.
 */
export const SYNTH_SAMPLE_RATE = 44100;

/** Phase-table width the tone functions index (CODAL `EMOJI_SYNTHESIZER_TONE_WIDTH`). */
const TONE_WIDTH = 1024;

/** Maximum tone-print amplitude (CODAL sampleRange, `setSampleRange(1023)`). */
const SAMPLE_RANGE = 1023;

/** Number of effect slots per segment (CODAL `EMOJI_SYNTHESIZER_TONE_EFFECTS`). */
const TONE_EFFECTS = 3;

/**
 * CODAL's `sineTone` table, vendored verbatim from codal-core
 * `Synthesizer.cpp`. `SineTone` indexes the first half directly and mirrors the
 * second half of the phase back onto it, so a 512-entry quarter/half table
 * covers the full tone width.
 */
const SINE_TONE: readonly number[] = [
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 11, 11, 12, 13,
  13, 14, 15, 16, 16, 17, 18, 19, 20, 21, 22, 22, 23, 24, 25, 26, 27, 28, 29, 30, 32, 33, 34, 35, 36, 37, 38, 40, 41,
  42, 43, 45, 46, 47, 49, 50, 51, 53, 54, 56, 57, 58, 60, 61, 63, 64, 66, 68, 69, 71, 72, 74, 76, 77, 79, 81, 82, 84,
  86, 87, 89, 91, 93, 95, 96, 98, 100, 102, 104, 106, 108, 110, 112, 114, 116, 118, 120, 122, 124, 126, 128, 130, 132,
  134, 136, 138, 141, 143, 145, 147, 149, 152, 154, 156, 158, 161, 163, 165, 167, 170, 172, 175, 177, 179, 182, 184,
  187, 189, 191, 194, 196, 199, 201, 204, 206, 209, 211, 214, 216, 219, 222, 224, 227, 229, 232, 235, 237, 240, 243,
  245, 248, 251, 253, 256, 259, 262, 264, 267, 270, 273, 275, 278, 281, 284, 287, 289, 292, 295, 298, 301, 304, 307,
  309, 312, 315, 318, 321, 324, 327, 330, 333, 336, 339, 342, 345, 348, 351, 354, 357, 360, 363, 366, 369, 372, 375,
  378, 381, 384, 387, 390, 393, 396, 399, 402, 405, 408, 411, 414, 417, 420, 424, 427, 430, 433, 436, 439, 442, 445,
  448, 452, 455, 458, 461, 464, 467, 470, 473, 477, 480, 483, 486, 489, 492, 495, 498, 502, 505, 508, 511, 514, 517,
  520, 524, 527, 530, 533, 536, 539, 542, 545, 549, 552, 555, 558, 561, 564, 567, 570, 574, 577, 580, 583, 586, 589,
  592, 595, 598, 602, 605, 608, 611, 614, 617, 620, 623, 626, 629, 632, 635, 638, 641, 644, 647, 650, 653, 656, 659,
  662, 665, 668, 671, 674, 677, 680, 683, 686, 689, 692, 695, 698, 701, 704, 707, 710, 713, 715, 718, 721, 724, 727,
  730, 733, 735, 738, 741, 744, 747, 749, 752, 755, 758, 760, 763, 766, 769, 771, 774, 777, 779, 782, 785, 787, 790,
  793, 795, 798, 800, 803, 806, 808, 811, 813, 816, 818, 821, 823, 826, 828, 831, 833, 835, 838, 840, 843, 845, 847,
  850, 852, 855, 857, 859, 861, 864, 866, 868, 870, 873, 875, 877, 879, 881, 884, 886, 888, 890, 892, 894, 896, 898,
  900, 902, 904, 906, 908, 910, 912, 914, 916, 918, 920, 922, 924, 926, 927, 929, 931, 933, 935, 936, 938, 940, 941,
  943, 945, 946, 948, 950, 951, 953, 954, 956, 958, 959, 961, 962, 964, 965, 966, 968, 969, 971, 972, 973, 975, 976,
  977, 979, 980, 981, 982, 984, 985, 986, 987, 988, 989, 990, 992, 993, 994, 995, 996, 997, 998, 999, 1000, 1000, 1001,
  1002, 1003, 1004, 1005, 1006, 1006, 1007, 1008, 1009, 1009, 1010, 1011, 1011, 1012, 1013, 1013, 1014, 1014, 1015,
  1015, 1016, 1016, 1017, 1017, 1018, 1018, 1019, 1019, 1019, 1020, 1020, 1020, 1021, 1021, 1021, 1021, 1022, 1022,
  1022, 1022, 1022, 1022, 1022, 1022, 1022, 1022, 1023, 1022,
];

/** Deterministic-noise multiplier CODAL's `NoiseTone` uses when its parameter is unset. */
const NOISE_MULTIPLIER = 7919;

/**
 * Evaluates a tone function at an integer phase `position` in `[0, TONE_WIDTH]`,
 * returning an amplitude in `[0, 1023]`. Ports CODAL's `Synthesizer` tone
 * functions: `SineTone` (mirrored table lookup), `SawtoothTone`,
 * `TriangleTone`, `SquareWaveTone` (hard 50% duty, the plain non-`Ext` form
 * `parseSoundExpression` wires), and the deterministic `NoiseTone`.
 */
export function tonePrint(waveform: SoundWaveform, position: number): number {
  switch (waveform) {
    case "sine": {
      const off = TONE_WIDTH - position;
      const index = off < TONE_WIDTH / 2 ? off : position;
      return SINE_TONE[index] as number;
    }
    case "sawtooth":
      return position;
    case "triangle":
      return position < 512 ? position * 2 : (1023 - position) * 2;
    case "square":
      return position < 512 ? 1023 : 0;
    case "noise":
      return (position * NOISE_MULTIPLIER) & 1023;
  }
}

/** The mutable per-segment synth state the effect functions drive (CODAL `synth->frequency`/`volume`). */
export interface SynthState {
  /** Live frequency in Hz, mutated by frequency-interpolation and vibrato/warble slots. */
  frequency: number;

  /** Live volume on CODAL's 0-1 scale, mutated by the volume-ramp and tremolo slots. */
  volume: number;
}

/**
 * Applies one effect slot's function at its current step, mutating {@link SynthState}
 * exactly as the corresponding CODAL `SoundSynthesizerEffects` function does. The
 * function reads the segment's base `frequency`/`volume` for interpolation
 * anchors and the slot's `param0` / `step` / `steps` / `progression`.
 *
 * @param slot - The effect slot (kind, steps, param0, optional progression).
 * @param state - The live synth state to mutate.
 * @param step - The current step index (fired before it is incremented).
 * @param baseFrequency - The segment's base frequency in Hz.
 * @param baseVolume - The segment's base volume on the 0-1 scale.
 */
export function applyEffect(
  slot: RawToneEffect,
  state: SynthState,
  step: number,
  baseFrequency: number,
  baseVolume: number
): void {
  const steps = Math.max(slot.steps, 1);
  switch (slot.kind) {
    case "none":
      return;
    case "linear": {
      const interval = (slot.param0 - baseFrequency) / steps;
      state.frequency = baseFrequency + interval * step;
      return;
    }
    case "logarithmic": {
      const s = step === 0 ? 1 : step;
      state.frequency = baseFrequency + (Math.log10(s) * (slot.param0 - baseFrequency)) / 1.95;
      if (state.frequency < 0) {
        state.frequency = 0;
      }
      return;
    }
    case "curve":
      // biome-ignore lint/suspicious/noApproximativeNumericConstant: CODAL curveInterpolation uses the literal 3.14159f; Math.PI would diverge from the device.
      state.frequency = Math.sin((step * 3.14159) / 180.0) * (slot.param0 - baseFrequency) + baseFrequency;
      return;
    case "exponential-rising":
      state.frequency = baseFrequency + Math.sin(0.01745329 * step) * slot.param0;
      return;
    case "exponential-falling":
      state.frequency = baseFrequency + Math.cos(0.01745329 * step) * slot.param0;
      return;
    case "arpeggio-ascending":
      state.frequency = progressionFrequency(baseFrequency, slot.progression, step);
      return;
    case "arpeggio-descending":
      state.frequency = progressionFrequency(baseFrequency, slot.progression, steps - step - 1);
      return;
    case "volume-ramp": {
      const delta = (slot.param0 - baseVolume) / steps;
      state.volume = baseVolume + step * delta;
      return;
    }
    case "frequency-vibrato":
      if (step === 0) {
        return;
      }
      state.frequency = step % 2 === 0 ? state.frequency / slot.param0 : state.frequency * slot.param0;
      return;
    case "volume-tremolo":
      if (step === 0) {
        return;
      }
      state.volume = step % 2 === 0 ? state.volume / slot.param0 : state.volume * slot.param0;
      return;
    case "warble":
      state.frequency = Math.sin(step) * (slot.param0 - baseFrequency) + baseFrequency;
      return;
  }
}

/** CODAL `calculateFrequencyFromProgression`: the note frequency at a progression offset. */
function progressionFrequency(root: number, progression: readonly number[] | undefined, offset: number): number {
  if (progression === undefined || progression.length === 0) {
    return root;
  }
  const octave = Math.trunc(offset / progression.length);
  const index = offset % progression.length;
  return root * 2 ** octave * (progression[index] as number);
}

/** Number of PCM samples CODAL's `determineSampleCount` renders for a segment duration. */
function determineSampleCount(durationMs: number): number {
  const seconds = Math.abs(durationMs) / 1000;
  return Math.trunc(SYNTH_SAMPLE_RATE * seconds);
}

/**
 * Renders one segment's samples into `output` starting at `writeOffset`, porting
 * CODAL's `SoundEmojiSynthesizer::fillOutputBuffer` sample loop: a phase
 * accumulator advanced by `skip = TONE_WIDTH * frequency / sampleRate`, the
 * three stepped effect slots fired at their step boundaries, and each sample
 * scaled by `gain = sampleRange * volume / 1024` and centered with
 * `offset = 512 - 512 * gain`.
 *
 * @returns The phase position after the segment, carried into the next segment.
 */
function renderSegment(fx: RawSoundEffect, output: Float32Array, writeOffset: number, position: number): number {
  const samplesToWrite = determineSampleCount(fx.durationMs);
  const state: SynthState = { frequency: fx.frequency, volume: fx.volume };

  const steps: number[] = [];
  const samplesPerStep: number[] = [];
  const step: number[] = [];
  for (let i = 0; i < TONE_EFFECTS; i++) {
    steps[i] = Math.max((fx.effects[i] as RawToneEffect).steps, 1);
    samplesPerStep[i] = samplesToWrite / (steps[i] as number);
    step[i] = 0;
  }

  let samplesWritten = 0;
  while (samplesWritten < samplesToWrite) {
    const skip = (TONE_WIDTH * state.frequency) / SYNTH_SAMPLE_RATE;
    const gain = (SAMPLE_RANGE * state.volume) / 1024.0;
    const offset = 512.0 - 512.0 * gain;

    const effectStepEnd: number[] = [];
    for (let i = 0; i < TONE_EFFECTS; i++) {
      effectStepEnd[i] = Math.trunc((samplesPerStep[i] as number) * (step[i] as number));
      if ((step[i] as number) === (steps[i] as number) - 1) {
        effectStepEnd[i] = samplesToWrite;
      }
    }
    let stepEndPosition = effectStepEnd[0] as number;
    for (let i = 1; i < TONE_EFFECTS; i++) {
      stepEndPosition = Math.min(stepEndPosition, effectStepEnd[i] as number);
    }

    while (samplesWritten < stepEndPosition) {
      const s = tonePrint(fx.waveform, Math.trunc(position));
      const raw = s * gain + offset;
      output[writeOffset + samplesWritten] = clampSample((raw - 512.0) / 512.0);
      samplesWritten++;
      position += skip;
      while (position > TONE_WIDTH) {
        position -= TONE_WIDTH;
      }
    }

    for (let i = 0; i < TONE_EFFECTS; i++) {
      if (samplesWritten === effectStepEnd[i] && (step[i] as number) < (steps[i] as number)) {
        applyEffect(fx.effects[i] as RawToneEffect, state, step[i] as number, fx.frequency, fx.volume);
        step[i] = (step[i] as number) + 1;
      }
    }
  }
  return position;
}

/**
 * Clamps a sample to the Web Audio `[-1, 1]` range. CODAL lets a volume-multiply
 * overshoot wrap through its `uint16_t` cast; the port clamps instead (a
 * deliberate, sim-only divergence from the device quirk).
 */
function clampSample(value: number): number {
  if (value > 1) {
    return 1;
  }
  if (value < -1) {
    return -1;
  }
  return value;
}

/**
 * Renders a decoded sound (its ordered raw effects) to mono PCM at
 * {@link SYNTH_SAMPLE_RATE}, a faithful port of CODAL's `SoundEmojiSynthesizer`.
 * Segments are concatenated back to back and the phase position carries across
 * them, matching the device's continuous oscillator. The output length is the
 * sum of each segment's CODAL sample count.
 *
 * @param effects - The decoded segments in play order (from `decodeSoundExpressionRaw`).
 * @returns Mono PCM samples in `[-1, 1]`.
 */
export function renderSoundToPcm(effects: readonly RawSoundEffect[]): Float32Array {
  let total = 0;
  for (const fx of effects) {
    total += determineSampleCount(fx.durationMs);
  }
  const output = new Float32Array(total);
  let position = 0;
  let writeOffset = 0;
  for (const fx of effects) {
    position = renderSegment(fx, output, writeOffset, position);
    writeOffset += determineSampleCount(fx.durationMs);
  }
  return output;
}

/**
 * Renders a speaker tone command to mono PCM at {@link SYNTH_SAMPLE_RATE} as a
 * single constant-pitch segment: the command's waveform sounded at its frequency
 * and volume for its duration, with no sweep, vibrato, or envelope. A command
 * with a zero or negative duration renders no samples.
 *
 * @param command - The tone accepted by the speaker port.
 * @returns Mono PCM samples in `[-1, 1]`.
 */
export function renderToneToPcm(command: SpeakerToneCommand): Float32Array {
  const flat: RawToneEffect = { kind: "none", steps: 0, param0: 0 };
  return renderSoundToPcm([
    {
      waveform: command.waveform,
      frequency: command.frequencyHz,
      volume: command.volume,
      durationMs: Math.max(command.durationMs, 0),
      effects: [flat, flat, flat],
    },
  ]);
}

/**
 * Renders a built-in sound emoji to PCM by name, or undefined for a name outside
 * the built-in set.
 *
 * @param name - A built-in sound name (for example `hello`).
 */
export function renderBuiltInSoundToPcm(name: string): Float32Array | undefined {
  const effects = decodeBuiltInSoundRaw(name);
  return effects === undefined ? undefined : renderSoundToPcm(effects);
}

/**
 * Renders every built-in sound to PCM, keyed by name. A Web Audio consumer builds
 * this once as a static lookup for all built-ins.
 */
export function renderAllBuiltInSoundsToPcm(): Map<string, Float32Array> {
  return new Map(BUILT_IN_SOUNDS.map((def) => [def.name, renderBuiltInSoundToPcm(def.name) as Float32Array]));
}
