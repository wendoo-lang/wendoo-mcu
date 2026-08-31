import {
  renderAllBuiltInSoundsToPcm,
  renderToneToPcm,
  type SpeakerPlayingSnapshot,
  type SpeakerToneCommand,
  SYNTH_SAMPLE_RATE,
} from "@wendoo/wodal/targets/microbit-v2";

/** The narrow slice of a Web Audio node the renderer connects and disconnects. */
export interface AudioNodeLike {
  connect(destination: AudioNodeLike): void;
  disconnect(): void;
}

/** The narrow slice of a Web Audio `AudioBuffer` the renderer fills with rendered PCM. */
export interface AudioBufferLike {
  readonly length: number;
  getChannelData(channel: number): Float32Array;
}

/** The narrow slice of a Web Audio `AudioBufferSourceNode` the renderer plays. */
export interface AudioBufferSourceNodeLike extends AudioNodeLike {
  buffer: AudioBufferLike | null;
  start(when?: number): void;
  stop(when?: number): void;
}

/**
 * The narrow slice of a Web Audio `AudioContext` the renderer uses. A real
 * `AudioContext` satisfies it structurally; a test mock records created buffers
 * and source nodes, since Web Audio is absent in the node/jsdom test
 * environment.
 */
export interface AudioContextLike {
  readonly currentTime: number;
  readonly sampleRate: number;
  readonly destination: AudioNodeLike;
  readonly state: string;
  createBuffer(numberOfChannels: number, length: number, sampleRate: number): AudioBufferLike;
  createBufferSource(): AudioBufferSourceNodeLike;
  resume(): Promise<void>;
  close(): Promise<void>;
}

/** Creates the audio context the renderer plays into. */
export type AudioContextFactory = () => AudioContextLike;

/** Master output gain applied to the full-scale PCM to keep playback at a comfortable level. */
const MASTER_GAIN = 0.25;

/** A sound currently playing on one device instance. */
interface ActiveVoice {
  /** The snapshot `playId` that started this voice; a new value marks a new play. */
  readonly playId: number;

  /** Stops the source node at once and disconnects it from the output. */
  readonly stop: () => void;
}

/**
 * App-owned Web Audio playback for the simulated micro:bit speaker. It plays the
 * device's built-in sound emoji and its plain tones audibly by rendering each to
 * PCM with WODAL's faithful CODAL synthesis port and playing it as a single
 * pre-rendered `AudioBuffer`, keyed off the speaker snapshot's `playId`.
 *
 * The renderer owns a single {@link AudioContextLike} shared by every device in
 * the fleet, with one active voice tracked per instance. Feed each instance's
 * `speaker.playing` snapshot every frame with {@link SpeakerAudio.sync}; a new
 * `playId` starts that sound, a cleared snapshot or a changed `playId` stops the
 * playing source (preempt, lease end, or reset).
 *
 * Browsers block audio until a user gesture: call {@link SpeakerAudio.unlock}
 * from the first click or keypress. A sound that would play before any gesture
 * is silently missed, matching browser autoplay policy.
 */
export class SpeakerAudio {
  private readonly createContext: AudioContextFactory;
  private readonly pcmByName: Map<string, Float32Array>;
  private readonly buffersByName = new Map<string, AudioBufferLike>();
  private readonly voices = new Map<string, ActiveVoice>();
  private context: AudioContextLike | undefined;

  /**
   * @param createContext - Factory for the shared audio context. Defaults to a
   *   real `AudioContext`; a test passes a recording mock.
   */
  constructor(createContext: AudioContextFactory = () => new AudioContext()) {
    this.createContext = createContext;
    this.pcmByName = renderAllBuiltInSoundsToPcm();
  }

  /**
   * Unlocks audio on a user gesture: lazily creates the shared audio context and
   * resumes it. Safe to call repeatedly and safe when Web Audio is unavailable
   * (it no-ops without throwing).
   */
  unlock(): void {
    try {
      this.context ??= this.createContext();
      if (this.context.state !== "running") {
        void this.context.resume();
      }
    } catch {
      // Web Audio unavailable or blocked; playback is silently skipped.
      this.context = undefined;
    }
  }

  /**
   * Reconciles one device instance's speaker snapshot with its playing source.
   * Starts the sound when `playing` carries a new `playId`, and stops the current
   * source when `playing` is undefined or its `playId` changed.
   *
   * @param instanceId - The device instance's id.
   * @param playing - The instance's current `speaker.playing` snapshot, or undefined when idle.
   */
  sync(instanceId: string, playing: SpeakerPlayingSnapshot | undefined): void {
    const active = this.voices.get(instanceId);
    if (playing === undefined) {
      if (active) {
        active.stop();
        this.voices.delete(instanceId);
      }
      return;
    }
    if (active && active.playId === playing.playId) {
      return;
    }
    if (active) {
      active.stop();
      this.voices.delete(instanceId);
    }
    this.startVoice(instanceId, playing);
  }

  /**
   * Stops and drops any voices for instances not in `instanceIds`. Call when the
   * fleet changes so a removed device's sound does not keep ringing.
   *
   * @param instanceIds - The ids of the instances that still exist.
   */
  retain(instanceIds: ReadonlySet<string>): void {
    for (const [id, voice] of this.voices) {
      if (!instanceIds.has(id)) {
        voice.stop();
        this.voices.delete(id);
      }
    }
  }

  /** Stops every playing source and closes the audio context. */
  dispose(): void {
    for (const voice of this.voices.values()) {
      voice.stop();
    }
    this.voices.clear();
    this.buffersByName.clear();
    if (this.context) {
      void this.context.close();
      this.context = undefined;
    }
  }

  /** Plays the snapshot's tone or named built-in and records the active voice. */
  private startVoice(instanceId: string, playing: SpeakerPlayingSnapshot): void {
    const context = this.context;
    if (context === undefined || context.state !== "running") {
      return;
    }
    const buffer =
      playing.tone === undefined ? this.bufferFor(context, playing.name) : this.toneBuffer(context, playing.tone);
    if (buffer === undefined) {
      return;
    }
    const source = context.createBufferSource();
    source.buffer = buffer;
    source.connect(context.destination);
    source.start(context.currentTime);
    this.voices.set(instanceId, {
      playId: playing.playId,
      stop: () => {
        try {
          source.stop(context.currentTime);
        } catch {
          // Already stopped.
        }
        try {
          source.disconnect();
        } catch {
          // Already disconnected.
        }
      },
    });
  }

  /**
   * Returns the cached playback buffer for a built-in name, building it once from
   * the rendered PCM. Undefined for a name outside the built-in set (a silent
   * no-op, matching the speaker port).
   */
  private bufferFor(context: AudioContextLike, name: string): AudioBufferLike | undefined {
    const cached = this.buffersByName.get(name);
    if (cached !== undefined) {
      return cached;
    }
    const pcm = this.pcmByName.get(name);
    if (pcm === undefined) {
      return undefined;
    }
    const buffer = playbackBuffer(context, pcm);
    if (buffer !== undefined) {
      this.buffersByName.set(name, buffer);
    }
    return buffer;
  }

  /**
   * Returns a freshly rendered playback buffer for one tone, synthesized by the
   * same port that renders the built-in sounds. Undefined for a tone that
   * renders no samples, such as a zero-duration tone (a silent no-op).
   */
  private toneBuffer(context: AudioContextLike, tone: SpeakerToneCommand): AudioBufferLike | undefined {
    return playbackBuffer(context, renderToneToPcm(tone));
  }
}

/**
 * Copies rendered PCM into a fresh single-channel playback buffer at the master
 * gain, or undefined when the PCM is empty (no buffer can be created for it).
 */
function playbackBuffer(context: AudioContextLike, pcm: Float32Array): AudioBufferLike | undefined {
  if (pcm.length === 0) {
    return undefined;
  }
  const buffer = context.createBuffer(1, pcm.length, SYNTH_SAMPLE_RATE);
  const data = buffer.getChannelData(0);
  for (let i = 0; i < pcm.length; i++) {
    data[i] = (pcm[i] as number) * MASTER_GAIN;
  }
  return buffer;
}
