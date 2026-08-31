import assert from "node:assert/strict";
import { describe, it } from "node:test";
import {
  mkSpeakerToneCommand,
  renderBuiltInSoundToPcm,
  renderToneToPcm,
  type SpeakerPlayingSnapshot,
  type SpeakerToneCommand,
} from "@wendoo/wodal/targets/microbit-v2";
import type { AudioBufferLike, AudioBufferSourceNodeLike, AudioContextLike, AudioNodeLike } from "./speaker-audio";
import { SpeakerAudio } from "./speaker-audio";

/** Base mock node tracking connect/disconnect. */
class MockNode implements AudioNodeLike {
  connected = 0;
  disconnected = false;
  connect(): void {
    this.connected++;
  }
  disconnect(): void {
    this.disconnected = true;
  }
}

/** A mock audio buffer exposing its writable channel data. */
class MockBuffer implements AudioBufferLike {
  readonly length: number;
  private readonly data: Float32Array;
  constructor(length: number) {
    this.length = length;
    this.data = new Float32Array(length);
  }
  getChannelData(): Float32Array {
    return this.data;
  }
}

class MockBufferSource extends MockNode implements AudioBufferSourceNodeLike {
  buffer: AudioBufferLike | null = null;
  started: number | undefined;
  stopped: number | undefined;
  start(when?: number): void {
    this.started = when;
  }
  stop(when?: number): void {
    this.stopped = when;
  }
}

/** A mock audio context recording every buffer and source it creates. */
class MockAudioContext implements AudioContextLike {
  currentTime = 0;
  readonly sampleRate = 48000;
  readonly destination = new MockNode();
  state = "suspended";
  resumeCalls = 0;
  closed = false;
  readonly buffers: MockBuffer[] = [];
  readonly bufferSources: MockBufferSource[] = [];
  createBuffer(_channels: number, length: number): AudioBufferLike {
    const buffer = new MockBuffer(length);
    this.buffers.push(buffer);
    return buffer;
  }
  createBufferSource(): AudioBufferSourceNodeLike {
    const node = new MockBufferSource();
    this.bufferSources.push(node);
    return node;
  }
  resume(): Promise<void> {
    this.resumeCalls++;
    this.state = "running";
    return Promise.resolve();
  }
  close(): Promise<void> {
    this.closed = true;
    return Promise.resolve();
  }
}

function playing(name: string, playId: number): SpeakerPlayingSnapshot {
  return { name, playId, startedAt: 0, durationMs: 1000 };
}

/** A speaker snapshot carrying a playing tone, as the tone port publishes it. */
function playingTone(tone: SpeakerToneCommand, playId: number): SpeakerPlayingSnapshot {
  return { name: "", tone, playId, startedAt: 0, durationMs: tone.durationMs };
}

/** The rendered PCM length microbit-sim's buffer for a built-in should carry. */
function pcmLength(name: string): number {
  const pcm = renderBuiltInSoundToPcm(name);
  assert.ok(pcm, `${name} should render`);
  return pcm.length;
}

describe("SpeakerAudio", () => {
  it("plays nothing before a user gesture unlocks audio", () => {
    const ctx = new MockAudioContext();
    const audio = new SpeakerAudio(() => ctx);
    audio.sync("d1", playing("hello", 1));
    assert.equal(ctx.bufferSources.length, 0);
  });

  it("resumes the context on unlock", () => {
    const ctx = new MockAudioContext();
    const audio = new SpeakerAudio(() => ctx);
    audio.unlock();
    assert.equal(ctx.resumeCalls, 1);
    assert.equal(ctx.state, "running");
  });

  it("plays a rendered buffer of the expected length via a buffer source on a new playId", () => {
    const ctx = new MockAudioContext();
    const audio = new SpeakerAudio(() => ctx);
    audio.unlock();
    audio.sync("d1", playing("hello", 1));
    assert.equal(ctx.bufferSources.length, 1);
    const source = ctx.bufferSources[0];
    assert.ok(source);
    assert.notEqual(source.buffer, null);
    assert.equal(source.buffer?.length, pcmLength("hello"));
    assert.notEqual(source.started, undefined);
    assert.equal(source.connected, 1);
  });

  it("does not replay while the same playId keeps playing", () => {
    const ctx = new MockAudioContext();
    const audio = new SpeakerAudio(() => ctx);
    audio.unlock();
    audio.sync("d1", playing("hello", 1));
    audio.sync("d1", playing("hello", 1));
    assert.equal(ctx.bufferSources.length, 1);
  });

  it("stops the prior source and starts the next when the playId changes (preempt)", () => {
    const ctx = new MockAudioContext();
    const audio = new SpeakerAudio(() => ctx);
    audio.unlock();
    audio.sync("d1", playing("hello", 1));
    const first = ctx.bufferSources[0];
    assert.ok(first);
    audio.sync("d1", playing("giggle", 2));
    assert.equal(first.disconnected, true);
    assert.notEqual(first.stopped, undefined);
    assert.equal(ctx.bufferSources.length, 2);
    const second = ctx.bufferSources[1];
    assert.ok(second);
    assert.equal(second.buffer?.length, pcmLength("giggle"));
    assert.notEqual(second.started, undefined);
  });

  it("stops the source when the speaker snapshot clears (lease end)", () => {
    const ctx = new MockAudioContext();
    const audio = new SpeakerAudio(() => ctx);
    audio.unlock();
    audio.sync("d1", playing("hello", 1));
    const source = ctx.bufferSources[0];
    assert.ok(source);
    audio.sync("d1", undefined);
    assert.equal(source.disconnected, true);
    assert.notEqual(source.stopped, undefined);
  });

  it("reuses one cached buffer across repeated plays of the same sound", () => {
    const ctx = new MockAudioContext();
    const audio = new SpeakerAudio(() => ctx);
    audio.unlock();
    audio.sync("d1", playing("hello", 1));
    audio.sync("d1", playing("hello", 2));
    assert.equal(ctx.bufferSources.length, 2);
    // The buffer is rendered once and reused for the second play.
    assert.equal(ctx.buffers.length, 1);
  });

  it("treats a name outside the built-in set as a silent no-op", () => {
    const ctx = new MockAudioContext();
    const audio = new SpeakerAudio(() => ctx);
    audio.unlock();
    audio.sync("d1", playing("not-a-sound", 1));
    assert.equal(ctx.bufferSources.length, 0);
  });

  it("plays a rendered tone buffer of the expected length when the snapshot carries a tone", () => {
    const ctx = new MockAudioContext();
    const audio = new SpeakerAudio(() => ctx);
    audio.unlock();
    const tone = mkSpeakerToneCommand("square", 880, 250, 1);
    audio.sync("d1", playingTone(tone, 1));
    assert.equal(ctx.bufferSources.length, 1);
    const source = ctx.bufferSources[0];
    assert.ok(source);
    assert.equal(source.buffer?.length, renderToneToPcm(tone).length);
    assert.notEqual(source.started, undefined);
    assert.equal(source.connected, 1);
  });

  it("fills a tone's buffer with audible samples", () => {
    const ctx = new MockAudioContext();
    const audio = new SpeakerAudio(() => ctx);
    audio.unlock();
    audio.sync("d1", playingTone(mkSpeakerToneCommand("square", 880, 250, 1), 1));
    const buffer = ctx.buffers[0];
    assert.ok(buffer);
    assert.ok(
      buffer.getChannelData().some((sample) => sample !== 0),
      "a sounding tone should render non-zero samples"
    );
  });

  it("plays a 0 Hz rest as a silent buffer of its full length", () => {
    const ctx = new MockAudioContext();
    const audio = new SpeakerAudio(() => ctx);
    audio.unlock();
    const rest = mkSpeakerToneCommand("square", 0, 250, 1);
    audio.sync("d1", playingTone(rest, 1));
    assert.equal(ctx.bufferSources.length, 1);
    const buffer = ctx.buffers[0];
    assert.ok(buffer);
    assert.equal(buffer.length, renderToneToPcm(rest).length);
    assert.ok(
      buffer.getChannelData().every((sample) => sample === 0),
      "a rest should render silence"
    );
  });

  it("treats a tone that renders no samples as a silent no-op", () => {
    const ctx = new MockAudioContext();
    const audio = new SpeakerAudio(() => ctx);
    audio.unlock();
    audio.sync("d1", playingTone(mkSpeakerToneCommand("sine", 440, 0, 1), 1));
    assert.equal(ctx.bufferSources.length, 0);
  });

  it("renders each tone play its own buffer", () => {
    const ctx = new MockAudioContext();
    const audio = new SpeakerAudio(() => ctx);
    audio.unlock();
    const short = mkSpeakerToneCommand("square", 880, 100, 1);
    const long = mkSpeakerToneCommand("square", 880, 400, 1);
    audio.sync("d1", playingTone(short, 1));
    audio.sync("d1", playingTone(long, 2));
    assert.equal(ctx.buffers.length, 2);
    assert.equal(ctx.buffers[0]?.length, renderToneToPcm(short).length);
    assert.equal(ctx.buffers[1]?.length, renderToneToPcm(long).length);
  });

  it("stops a playing built-in when a tone preempts it", () => {
    const ctx = new MockAudioContext();
    const audio = new SpeakerAudio(() => ctx);
    audio.unlock();
    audio.sync("d1", playing("hello", 1));
    const first = ctx.bufferSources[0];
    assert.ok(first);
    audio.sync("d1", playingTone(mkSpeakerToneCommand("triangle", 660, 200, 1), 2));
    assert.equal(first.disconnected, true);
    assert.notEqual(first.stopped, undefined);
    assert.equal(ctx.bufferSources.length, 2);
  });

  it("drops voices for instances no longer retained", () => {
    const ctx = new MockAudioContext();
    const audio = new SpeakerAudio(() => ctx);
    audio.unlock();
    audio.sync("d1", playing("hello", 1));
    const source = ctx.bufferSources[0];
    assert.ok(source);
    audio.retain(new Set(["d2"]));
    assert.equal(source.disconnected, true);
  });

  it("closes the context and clears buffers on dispose", () => {
    const ctx = new MockAudioContext();
    const audio = new SpeakerAudio(() => ctx);
    audio.unlock();
    audio.sync("d1", playing("hello", 1));
    audio.dispose();
    assert.equal(ctx.closed, true);
  });
});
