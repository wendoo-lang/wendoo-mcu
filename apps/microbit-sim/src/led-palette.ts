/**
 * Fill color of an LED of the given brightness, on the micro:bit display's own
 * red palette. An unlit LED reads as a dark red recess; a lit one takes the
 * display red at an alpha rising with brightness.
 *
 * @param brightness - LED brightness, 0 (unlit) to 255 (full).
 */
export function ledColor(brightness: number): string {
  if (brightness <= 0) {
    return "rgba(120, 20, 20, 0.35)";
  }
  const intensity = 0.3 + (0.7 * brightness) / 255;
  return `rgba(239, 68, 68, ${intensity.toFixed(3)})`;
}

/** Blur radius, in CSS pixels, of the halo a full-brightness display LED casts. */
const kDeviceGlowBlur = 4;

/** Spread radius, in CSS pixels, every lit display LED's halo is drawn at. */
const kDeviceGlowSpread = 1.5;

/** Alpha the halo of a full-brightness display LED is drawn at. */
const kDeviceGlowAlpha = 0.3;

/**
 * The `box-shadow` an LED of the given brightness casts on the simulated
 * device's display: a halo in the display red at a fixed
 * {@link kDeviceGlowSpread}px spread, its blur and alpha scaled by the
 * brightness. An unlit LED casts none and yields `undefined`.
 *
 * @param brightness - LED brightness, 0 (unlit) to 255 (full).
 */
export function deviceLedGlow(brightness: number): string | undefined {
  if (brightness <= 0) {
    return undefined;
  }
  const scale = brightness / 255;
  const blur = (kDeviceGlowBlur * scale).toFixed(2);
  const alpha = (kDeviceGlowAlpha * scale).toFixed(3);
  return `0 0 ${blur}px ${kDeviceGlowSpread}px rgba(239, 68, 68, ${alpha})`;
}
