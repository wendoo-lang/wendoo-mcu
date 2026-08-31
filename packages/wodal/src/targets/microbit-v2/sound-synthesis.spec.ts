import assert from "node:assert/strict";
import { describe, test } from "node:test";
import { mkSpeakerToneCommand } from "./microbit-speaker";
import { decodeBuiltInSoundRaw, decodeSoundExpressionRaw, type RawToneEffect } from "./sound-expression";
import {
  applyEffect,
  renderBuiltInSoundToPcm,
  renderSoundToPcm,
  renderToneToPcm,
  SYNTH_SAMPLE_RATE,
  type SynthState,
  tonePrint,
} from "./sound-synthesis";
import { BUILT_IN_SOUNDS } from "./wendoo/built-in-sounds";

/** CODAL's per-segment sample count: `(int)(sampleRate * ms/1000)`, truncated. */
function codalSampleCount(durationMs: number): number {
  return Math.trunc((SYNTH_SAMPLE_RATE * Math.abs(durationMs)) / 1000);
}

/** Asserts two numbers agree within an absolute tolerance (float-precision slack vs CODAL). */
function assertClose(actual: number, expected: number, epsilon: number, label: string): void {
  assert.ok(Math.abs(actual - expected) <= epsilon, `${label}: ${actual} != ${expected} (+/- ${epsilon})`);
}

/** Applies an effect slot to a fresh synth state and returns the mutated state. */
function evaluate(slot: RawToneEffect, step: number, baseFrequency: number, baseVolume: number): SynthState {
  const state: SynthState = { frequency: baseFrequency, volume: baseVolume };
  applyEffect(slot, state, step, baseFrequency, baseVolume);
  return state;
}

describe("sound-synthesis PCM port", () => {
  test("renders round(down) 44100*ms/1000 samples per segment, concatenated", () => {
    for (const def of BUILT_IN_SOUNDS) {
      const effects = decodeSoundExpressionRaw(def.encoded);
      const expected = effects.reduce((sum, fx) => sum + codalSampleCount(fx.durationMs), 0);
      assert.equal(renderBuiltInSoundToPcm(def.name)?.length, expected, def.name);
    }
  });

  test("a single-segment sound's length is its exact CODAL sample count", () => {
    // A 500 ms sine segment (no frequency sweep, no modulation).
    const [fx] = decodeSoundExpressionRaw("010000500050000000000000001000000000000000000000000000000000000000000000");
    assert.ok(fx);
    assert.equal(fx.durationMs, 500);
    assert.equal(renderSoundToPcm([fx]).length, codalSampleCount(500));
    assert.equal(codalSampleCount(500), 22050);
  });

  test("every rendered sample stays within the Web Audio [-1, 1] range", () => {
    for (const def of BUILT_IN_SOUNDS) {
      const pcm = renderBuiltInSoundToPcm(def.name);
      assert.ok(pcm);
      for (const sample of pcm) {
        assert.ok(sample >= -1 && sample <= 1, `${def.name}: ${sample} out of range`);
      }
    }
  });
});

describe("sound-synthesis tone rendering", () => {
  test("a tone renders its CODAL sample count as one constant-pitch segment", () => {
    const pcm = renderToneToPcm(mkSpeakerToneCommand("square", 440, 250, 1));
    assert.equal(pcm.length, codalSampleCount(250));
    // A constant-pitch square at full volume swings between the tone print's
    // two levels; nothing sweeps or fades across the segment.
    assert.ok(pcm.some((sample) => sample > 0.9));
    assert.ok(pcm.some((sample) => sample < -0.9));
  });

  test("each wave shape renders through its own CODAL tone function", () => {
    // CODAL's full-volume gain: sampleRange (1023) / 1024, centered on 512.
    const fullGain = 1023 / 1024;
    for (const waveform of ["square", "sawtooth", "sine", "triangle"] as const) {
      const pcm = renderToneToPcm(mkSpeakerToneCommand(waveform, 440, 100, 1));
      assert.equal(pcm.length, codalSampleCount(100));
      assertClose(pcm[0] as number, ((tonePrint(waveform, 0) - 512) * fullGain) / 512, 1e-6, waveform);
    }
  });

  test("a silent rest and a zero-length tone render no signal", () => {
    const rest = renderToneToPcm(mkSpeakerToneCommand("square", 0, 100, 1));
    assert.equal(rest.length, codalSampleCount(100));
    for (const sample of rest) {
      assert.equal(sample, 0);
    }
    assert.equal(renderToneToPcm(mkSpeakerToneCommand("square", 440, 0, 1)).length, 0);
  });
});

describe("sound-synthesis tone functions match CODAL", () => {
  test("sine mirrors the vendored table across the tone width", () => {
    assert.equal(tonePrint("sine", 0), 0);
    assert.equal(tonePrint("sine", 128), 149);
    assert.equal(tonePrint("sine", 512), 1023);
    // The upper half mirrors: position p and (1024 - p) index the same entry.
    assert.equal(tonePrint("sine", 600), tonePrint("sine", 424));
    assert.equal(tonePrint("sine", 1024), tonePrint("sine", 0));
  });

  test("sawtooth returns the phase position", () => {
    assert.equal(tonePrint("sawtooth", 0), 0);
    assert.equal(tonePrint("sawtooth", 511), 511);
    assert.equal(tonePrint("sawtooth", 1023), 1023);
  });

  test("triangle rises then falls", () => {
    assert.equal(tonePrint("triangle", 0), 0);
    assert.equal(tonePrint("triangle", 100), 200);
    assert.equal(tonePrint("triangle", 512), (1023 - 512) * 2);
    assert.equal(tonePrint("triangle", 1000), (1023 - 1000) * 2);
  });

  test("square is a hard 50% duty wave", () => {
    assert.equal(tonePrint("square", 0), 1023);
    assert.equal(tonePrint("square", 511), 1023);
    assert.equal(tonePrint("square", 512), 0);
    assert.equal(tonePrint("square", 1023), 0);
  });

  test("noise is deterministic (position*7919) & 1023", () => {
    assert.equal(tonePrint("noise", 1), (1 * 7919) & 1023);
    assert.equal(tonePrint("noise", 2), (2 * 7919) & 1023);
    assert.equal(tonePrint("noise", 1), 751);
    assert.equal(tonePrint("noise", 2), 478);
    // Deterministic: same position always yields the same amplitude.
    assert.equal(tonePrint("noise", 37), tonePrint("noise", 37));
  });
});

describe("sound-synthesis effect functions match hand-computed CODAL values", () => {
  test("linear interpolation is a stepped ramp toward the end frequency", () => {
    const slot: RawToneEffect = { kind: "linear", steps: 4, param0: 200 };
    assert.equal(evaluate(slot, 0, 100, 1).frequency, 100);
    assert.equal(evaluate(slot, 2, 100, 1).frequency, 150);
    assert.equal(evaluate(slot, 4, 100, 1).frequency, 200);
  });

  test("logarithmic interpolation hacks step 0 to 1 and scales by log10/1.95", () => {
    const slot: RawToneEffect = { kind: "logarithmic", steps: 4, param0: 200 };
    assert.equal(evaluate(slot, 0, 100, 1).frequency, 100);
    assert.equal(evaluate(slot, 1, 100, 1).frequency, 100);
    assertClose(evaluate(slot, 2, 100, 1).frequency, 100 + (Math.log10(2) * 100) / 1.95, 1e-6, "log step 2");
  });

  test("curve interpolation follows sin(step*pi/180)", () => {
    const slot: RawToneEffect = { kind: "curve", steps: 90, param0: 200 };
    // biome-ignore lint/suspicious/noApproximativeNumericConstant: matches CODAL curveInterpolation's literal 3.14159f.
    assertClose(evaluate(slot, 90, 100, 1).frequency, Math.sin((90 * 3.14159) / 180) * 100 + 100, 1e-4, "curve at 90");
  });

  test("exponential rising/falling use sin/cos(0.01745329*step) as an amplitude", () => {
    const rising: RawToneEffect = { kind: "exponential-rising", steps: 90, param0: 50 };
    const falling: RawToneEffect = { kind: "exponential-falling", steps: 90, param0: 50 };
    assertClose(evaluate(rising, 90, 100, 1).frequency, 100 + Math.sin(0.01745329 * 90) * 50, 1e-4, "rising 90");
    assert.equal(evaluate(falling, 0, 100, 1).frequency, 150);
  });

  test("warble jumps by sin(step) between base and end frequency", () => {
    const slot: RawToneEffect = { kind: "warble", steps: 700, param0: 200 };
    assert.equal(evaluate(slot, 0, 100, 1).frequency, 100);
    assertClose(evaluate(slot, 1, 100, 1).frequency, Math.sin(1) * 100 + 100, 1e-6, "warble step 1");
  });

  test("ascending arpeggio walks the progression, octave-shifting past its length", () => {
    // Major scale on a 100 Hz root: interval[2] = 1.25, and index 7 wraps to octave 1 * interval[0].
    const raw = decodeSoundExpressionRaw("010000100100008000000000001000000800000000000000000000000000000000000000")[0];
    assert.ok(raw);
    const slot = raw.effects[0];
    assert.equal(slot.kind, "arpeggio-ascending");
    assertClose(evaluate(slot, 2, 100, 1).frequency, 125, 1e-3, "arp step 2");
    assertClose(evaluate(slot, 7, 100, 1).frequency, 200, 1e-3, "arp step 7 (octave up)");
  });

  test("volume ramp is a 36-step linear ramp to the end volume", () => {
    const slot: RawToneEffect = { kind: "volume-ramp", steps: 36, param0: 1 };
    assert.equal(evaluate(slot, 0, 100, 0.5).volume, 0.5);
    assertClose(evaluate(slot, 18, 100, 0.5).volume, 0.75, 1e-9, "ramp midpoint");
    assertClose(evaluate(slot, 36, 100, 0.5).volume, 1, 1e-9, "ramp end");
  });

  test("frequency vibrato multiplies on odd steps and divides on even steps", () => {
    const slot: RawToneEffect = { kind: "frequency-vibrato", steps: 10, param0: 2 };
    // Step 0 is a no-op; the live frequency carries between steps.
    const state: SynthState = { frequency: 100, volume: 1 };
    applyEffect(slot, state, 0, 100, 1);
    assert.equal(state.frequency, 100);
    applyEffect(slot, state, 1, 100, 1);
    assert.equal(state.frequency, 200);
    applyEffect(slot, state, 2, 100, 1);
    assert.equal(state.frequency, 100);
  });

  test("volume tremolo multiplies on odd steps and divides on even steps", () => {
    const slot: RawToneEffect = { kind: "volume-tremolo", steps: 10, param0: 2 };
    const state: SynthState = { frequency: 100, volume: 0.5 };
    applyEffect(slot, state, 0, 100, 0.5);
    assert.equal(state.volume, 0.5);
    applyEffect(slot, state, 1, 100, 0.5);
    assert.equal(state.volume, 1);
    applyEffect(slot, state, 2, 100, 0.5);
    assert.equal(state.volume, 0.5);
  });
});

describe("sound-synthesis golden fingerprint", () => {
  /** A stable FNV-1a hash over quantized samples; any change to the port shifts it. */
  function fingerprint(pcm: Float32Array): number {
    let h = 2166136261 >>> 0;
    for (let i = 0; i < pcm.length; i++) {
      const q = Math.round((pcm[i] as number) * 10000) & 0xffff;
      h = (h ^ q) >>> 0;
      h = Math.imul(h, 16777619) >>> 0;
    }
    return h >>> 0;
  }

  test("giggle renders a deterministic, pinned PCM waveform", () => {
    const pcm = renderBuiltInSoundToPcm("giggle");
    assert.ok(pcm);
    assert.equal(pcm.length, 65752);
    assert.equal(fingerprint(pcm), 897552483);
  });

  test("hello renders a deterministic, pinned PCM waveform", () => {
    const pcm = renderBuiltInSoundToPcm("hello");
    assert.ok(pcm);
    assert.equal(pcm.length, 22313);
    assert.equal(fingerprint(pcm), 165441694);
  });

  test("decodeBuiltInSoundRaw resolves the built-in set and rejects unknown names", () => {
    assert.equal(decodeBuiltInSoundRaw("giggle")?.length, 5);
    assert.equal(decodeBuiltInSoundRaw("hello")?.length, 3);
    assert.equal(decodeBuiltInSoundRaw("bogus"), undefined);
    assert.equal(renderBuiltInSoundToPcm("bogus"), undefined);
  });
});
