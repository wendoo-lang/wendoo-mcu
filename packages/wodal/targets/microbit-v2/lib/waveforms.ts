/**
 * The wave shapes the micro:bit's speaker sounds a tone with, keyed by name.
 * Pass a member as the `waveform` option of `ctx.microbit.audio.playTone`, for
 * example
 * `await ctx.microbit.audio.playTone(440, { waveform: waveforms.sine })`.
 */
export const waveforms = {
  square: "square",
  sawtooth: "sawtooth",
  sine: "sine",
  triangle: "triangle",
} as const;
