import { OperationEnd, type OperationEndListener } from "../../core/operation-end";
import { findBuiltInSound } from "./wendoo/built-in-sounds";

/** Lowest tone pitch the speaker port accepts, in Hz. A 0 Hz tone is a silent rest. */
export const MIN_TONE_FREQUENCY_HZ = 0;

/** Highest tone pitch the speaker port accepts, in Hz. */
export const MAX_TONE_FREQUENCY_HZ = 9999;

/** Oscillator wave shape of a plain constant-pitch tone. */
export type SpeakerToneWaveform = "square" | "sawtooth" | "sine" | "triangle";

/** Every wave shape the speaker port sounds a tone with. */
const TONE_WAVEFORMS: readonly SpeakerToneWaveform[] = ["square", "sawtooth", "sine", "triangle"];

/**
 * Looks up a tone wave shape by name. Returns undefined for a name outside
 * {@link SpeakerToneWaveform}.
 *
 * @param name - Wave shape name to resolve.
 */
export function findToneWaveform(name: string): SpeakerToneWaveform | undefined {
  return TONE_WAVEFORMS.find((waveform) => waveform === name);
}

/**
 * A plain constant-pitch tone as accepted by the speaker port: one waveform at
 * one frequency for one duration. Build it with {@link mkSpeakerToneCommand}.
 */
export interface SpeakerToneCommand {
  /** Oscillator wave shape the tone is sounded with. */
  readonly waveform: SpeakerToneWaveform;

  /**
   * Pitch in Hz, clamped into {@link MIN_TONE_FREQUENCY_HZ} to
   * {@link MAX_TONE_FREQUENCY_HZ}. Zero is a rest and carries a zero
   * {@link volume}.
   */
  readonly frequencyHz: number;

  /**
   * Play length in whole milliseconds. A negative length marks a dropped
   * segment: {@link MicroBitSpeaker.playTone} plays nothing and ends the call at
   * once.
   */
  readonly durationMs: number;

  /** Volume as a fraction of full tone volume, clamped into 0 to 1. */
  readonly volume: number;
}

/**
 * Builds the tone command the speaker port accepts, clamping the pitch into
 * {@link MIN_TONE_FREQUENCY_HZ} to {@link MAX_TONE_FREQUENCY_HZ} and the volume
 * into 0 to 1. A 0 Hz tone is encoded as a silent rest: its volume is 0. The
 * duration is returned unchanged, sign included.
 *
 * @param waveform - Wave shape to sound the tone with.
 * @param frequencyHz - Requested pitch in Hz, before clamping.
 * @param durationMs - Play length in whole milliseconds; may be negative.
 * @param volume - Requested volume as a fraction of full, before clamping.
 */
export function mkSpeakerToneCommand(
  waveform: SpeakerToneWaveform,
  frequencyHz: number,
  durationMs: number,
  volume: number
): SpeakerToneCommand {
  const clampedFrequency = Math.min(Math.max(frequencyHz, MIN_TONE_FREQUENCY_HZ), MAX_TONE_FREQUENCY_HZ);
  const clampedVolume = Math.min(Math.max(volume, 0), 1);
  return {
    waveform,
    frequencyHz: clampedFrequency,
    durationMs,
    volume: clampedFrequency === MIN_TONE_FREQUENCY_HZ ? 0 : clampedVolume,
  };
}

/** The sound currently holding the speaker lease, as exposed to app adapters. */
export interface SpeakerPlayingSnapshot {
  /** Name of the playing built-in sound; the empty string while a tone plays. */
  readonly name: string;

  /** The playing tone; absent while a built-in sound plays. */
  readonly tone?: SpeakerToneCommand;

  /** Logical tick time at which the play began. */
  readonly startedAt: number;

  /** Nominal total duration in milliseconds; the lease runs to `startedAt + durationMs`. */
  readonly durationMs: number;

  /** Monotonic per-play nonce; a new value marks a new play. */
  readonly playId: number;
}

/** Snapshot of the speaker state exposed to app adapters. */
export interface MicroBitSpeakerSnapshot {
  /** The playing sound, or undefined while the speaker is idle. */
  readonly playing: SpeakerPlayingSnapshot | undefined;
}

/** A play holding the speaker lease until its nominal duration elapses. */
interface ActivePlay {
  /** Name of the playing built-in sound; the empty string for a tone. */
  readonly name: string;

  /** The playing tone, or undefined for a built-in sound. */
  readonly tone: SpeakerToneCommand | undefined;

  /** Logical tick time at which the play began. */
  readonly startedAt: number;

  /** Nominal total duration in milliseconds. */
  readonly durationMs: number;

  /** Monotonic per-play nonce. */
  readonly playId: number;

  /** Invoked once with how the play ended. */
  readonly onEnd: OperationEndListener;
}

/**
 * CODAL-style speaker facade: a single sound output leased by the playing
 * built-in sound or tone for its nominal duration against logical tick time,
 * mirroring the display lease mechanics.
 */
export class MicroBitSpeaker {
  /** The play holding the speaker lease, or undefined while idle. */
  private activePlay: ActivePlay | undefined;

  /** Per-play nonce source; increments on every accepted play. */
  private nextPlayId = 0;

  /**
   * Starts an asynchronous built-in sound play requested at logical tick time
   * `requestTime`. An accepted play takes the speaker lease for the sound's
   * nominal total duration; the lease is settled by {@link advancePlay} and
   * `onEnd` fires once the duration has elapsed. When the speaker is already
   * busy the new play is dropped: nothing plays and `onEnd` fires at once with
   * {@link OperationEnd.Dropped}, so the dispatching fiber continues without
   * blocking. A name outside the built-in set is dropped the same way: nothing
   * plays and no lease is taken.
   *
   * @param name - Name of the built-in sound to play.
   * @param requestTime - Logical tick time the play was requested.
   * @param onEnd - Invoked once with how the play ended, at the moment it ends.
   */
  playSoundEmoji(name: string, requestTime: number, onEnd: OperationEndListener): void {
    if (this.activePlay !== undefined) {
      onEnd(OperationEnd.Dropped);
      return;
    }
    const def = findBuiltInSound(name);
    if (def === undefined) {
      onEnd(OperationEnd.Dropped);
      return;
    }
    this.nextPlayId += 1;
    this.activePlay = {
      name,
      tone: undefined,
      startedAt: requestTime,
      durationMs: def.durationMs,
      playId: this.nextPlayId,
      onEnd,
    };
  }

  /**
   * Starts an asynchronous tone play requested at logical tick time
   * `requestTime`. An accepted tone takes the speaker lease for
   * `command.durationMs`; the lease is settled by {@link advancePlay} and
   * `onEnd` fires once the duration has elapsed. When the speaker is already
   * busy the tone is dropped: nothing sounds and `onEnd` fires at once with
   * {@link OperationEnd.Dropped}, so the dispatching fiber continues without
   * blocking. A negative duration is dropped the same way. A zero duration is
   * accepted and ends on the next lease settle.
   *
   * @param command - The clamped tone to sound (see {@link mkSpeakerToneCommand}).
   * @param requestTime - Logical tick time the play was requested.
   * @param onEnd - Invoked once with how the play ended, at the moment it ends.
   */
  playTone(command: SpeakerToneCommand, requestTime: number, onEnd: OperationEndListener): void {
    if (this.activePlay !== undefined || command.durationMs < 0) {
      onEnd(OperationEnd.Dropped);
      return;
    }
    this.nextPlayId += 1;
    this.activePlay = {
      name: "",
      tone: command,
      startedAt: requestTime,
      durationMs: command.durationMs,
      playId: this.nextPlayId,
      onEnd,
    };
  }

  /** True while a play holds the speaker lease. */
  isBusy(): boolean {
    return this.activePlay !== undefined;
  }

  /**
   * Releases the current speaker lease at once: the held play ends as
   * {@link OperationEnd.Preempted}. A no-op when no lease is held.
   */
  preempt(): void {
    const play = this.activePlay;
    if (play === undefined) {
      return;
    }
    this.activePlay = undefined;
    play.onEnd(OperationEnd.Preempted);
  }

  /**
   * Completes the active play (firing `onEnd`) once its nominal duration has
   * elapsed by `now`. This is the per-think speaker poll: it settles the play
   * holding the lease.
   *
   * @param now - Current logical tick time.
   */
  advancePlay(now: number): void {
    const play = this.activePlay;
    if (play === undefined || now < play.startedAt + play.durationMs) {
      return;
    }
    this.activePlay = undefined;
    play.onEnd(OperationEnd.Completed);
  }

  /**
   * Resets the speaker to its power-on state: drops any held play without
   * firing its `onEnd`. Call whenever the device timer resets.
   */
  reset(): void {
    this.activePlay = undefined;
  }

  /** Returns a serializable view of the speaker state. */
  snapshot(): MicroBitSpeakerSnapshot {
    const play = this.activePlay;
    if (play === undefined) {
      return { playing: undefined };
    }
    const playing = {
      name: play.name,
      startedAt: play.startedAt,
      durationMs: play.durationMs,
      playId: play.playId,
    };
    return { playing: play.tone === undefined ? playing : { ...playing, tone: play.tone } };
  }
}
