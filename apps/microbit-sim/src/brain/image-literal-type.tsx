import type { CustomLiteralType } from "@wendoo/ui";
import {
  imageDigitsFromValue,
  imageGridBytes,
  imageLevelBrightness,
  imageValueFromDigits,
  isImageGridDigits,
  WODAL_SHARED_TYPE_IDS,
} from "@wendoo/wodal";
import { MICROBIT_LED_MATRIX_SIZE } from "@wendoo/wodal/targets/microbit-v2";
import { type CSSProperties, useState } from "react";
import { ledColor } from "@/led-palette";

/** Side length, in pixels, of the grid an image literal is drawn on. */
const kGridSize = MICROBIT_LED_MATRIX_SIZE;

/** Number of pixels the grid holds. */
const kGridPixelCount = kGridSize * kGridSize;

/** Number of brightness levels a pixel takes, one per hex digit. */
const kLevelCount = 16;

/** Key the grid's pixel digits travel under in a create-literal dialog's input state. */
export const kImagePixelsFieldKey = "pixels";

/** The all-unlit grid. */
const kBlankGridDigits = "0".repeat(kGridPixelCount);

/**
 * The pixel digits a create-literal dialog's input state names: one hex
 * brightness digit per pixel, row-major, {@link kGridPixelCount} of them. An
 * input state carrying no digits names the unlit grid; a state carrying
 * anything else yields `undefined`.
 *
 * @param state - Input state of a create-literal dialog.
 */
export function imageGridDigits(state: Record<string, string>): string | undefined {
  const digits = state[kImagePixelsFieldKey] ?? "";
  if (digits === "") {
    return kBlankGridDigits;
  }
  return isImageGridDigits(digits) ? digits : undefined;
}

/** Halo a lit LED casts, in CSS pixels and alpha, at full brightness. */
interface LedGlow {
  /** Blur radius of the halo. */
  blur: number;
  /** Spread radius of the halo; a lit LED never spreads less than 1px. */
  spread: number;
  /** Alpha the halo is drawn at. */
  alpha: number;
}

/** Shape one LED is drawn in: a portrait rectangle plus the halo it casts. */
interface LedGeometry {
  /** LED width, in CSS pixels. */
  width: number;
  /** LED height, in CSS pixels; greater than the width, as on the device. */
  height: number;
  /** Corner radius, in CSS pixels. */
  radius: number;
  /** Halo the LED casts at full brightness. */
  glow: LedGlow;
}

/** The LED a grid-editor well holds, and the tap target it is centered in. */
const kEditorLed: LedGeometry = { width: 20, height: 30, radius: 3, glow: { blur: 16, spread: 4, alpha: 0.55 } };

/** Side of the square tap target a grid-editor LED is centered in, in CSS pixels. */
const kEditorWellSize = 40;

/** The LED a placed tile's preview well holds. */
const kPreviewLed: LedGeometry = { width: 4, height: 6, radius: 1.5, glow: { blur: 6, spread: 1.5, alpha: 0.45 } };

/** Side of the square well a preview LED is centered in, in CSS pixels. */
const kPreviewWellSize = 7;

/** Gap, in CSS pixels, between adjacent wells of the preview grid. */
const kPreviewWellGap = 1;

/** Padding, in CSS pixels, the preview grid stands its wells inside. */
const kPreviewGridPadding = 1.5;

/**
 * The `box-shadow` an LED of `brightness` (0-255) casts, scaled from `glow`'s
 * full-brightness figures. An unlit LED casts none and yields `undefined`.
 */
function ledGlowShadow(brightness: number, glow: LedGlow): string | undefined {
  if (brightness <= 0) {
    return undefined;
  }
  const scale = brightness / 255;
  const blur = (glow.blur * scale).toFixed(2);
  const spread = Math.max(1, glow.spread * scale).toFixed(2);
  const alpha = (glow.alpha * scale).toFixed(3);
  return `0 0 ${blur}px ${spread}px rgba(239, 68, 68, ${alpha})`;
}

/** The style an LED of `brightness` (0-255) is drawn with at `geometry`. */
function ledStyle(brightness: number, geometry: LedGeometry): CSSProperties {
  return {
    width: `${geometry.width}px`,
    height: `${geometry.height}px`,
    borderRadius: `${geometry.radius}px`,
    backgroundColor: ledColor(brightness),
    boxShadow: ledGlowShadow(brightness, geometry.glow),
  };
}

/** The style of the square well an LED of `size` CSS pixels is centered in. */
function wellStyle(size: number): CSSProperties {
  return { width: `${size}px`, height: `${size}px` };
}

/** The key of the grid cell at `index`, naming its row and column. */
function cellKey(index: number): string {
  return `r${Math.trunc(index / kGridSize)}c${index % kGridSize}`;
}

/**
 * The pixel digits a tap on the pixel at `index` produces while brightness
 * `level` is selected: a lit pixel goes unlit, and an unlit one takes the
 * level. Every other pixel keeps its digit.
 *
 * @param digits - The grid's current pixel digits, one hex digit per pixel.
 * @param index - Row-major index of the tapped pixel.
 * @param level - Selected brightness level, 0 to 15.
 */
export function paintGridDigit(digits: string, index: number, level: number): string {
  const painted = digits.charAt(index) === "0" ? level : 0;
  return digits.slice(0, index) + painted.toString(16) + digits.slice(index + 1);
}

/** What the grid editor draws and reports on. */
interface ImageGridEditorProps {
  /** The pixel digits the grid currently draws, one hex digit per pixel. */
  digits: string;
  /** Called with the pixel digits each paint produces. */
  onChange: (digits: string) => void;
}

/**
 * The 5x5 grid an image literal is drawn on, plus the slider its brightness is
 * picked with. Tapping a pixel toggles it: a lit one goes out, and an unlit one
 * lights at the slider's level.
 */
function ImageGridEditor({ digits, onChange }: ImageGridEditorProps) {
  const [level, setLevel] = useState(kLevelCount - 1);

  return (
    <div className="grid justify-items-center gap-4">
      <div data-testid="image-literal-grid" className="grid grid-cols-5 gap-0.5 rounded-lg bg-panel p-2">
        {[...digits].map((digit, index) => (
          <button
            key={cellKey(index)}
            type="button"
            data-testid="image-literal-cell"
            data-index={index}
            data-level={digit}
            aria-label={`Pixel row ${Math.trunc(index / kGridSize) + 1} column ${(index % kGridSize) + 1}`}
            className="flex items-center justify-center rounded-md"
            style={wellStyle(kEditorWellSize)}
            onClick={() => onChange(paintGridDigit(digits, index, level))}
          >
            <span style={ledStyle(imageLevelBrightness(Number.parseInt(digit, 16)), kEditorLed)} />
          </button>
        ))}
      </div>
      <div className="grid w-full grid-cols-[1fr_auto] items-center gap-4">
        <input
          type="range"
          data-testid="image-literal-brightness"
          min={0}
          max={kLevelCount - 1}
          step={1}
          value={level}
          aria-label="Brightness"
          className="w-full"
          style={{ accentColor: "var(--color-primary)" }}
          onChange={(event) => setLevel(Number.parseInt(event.target.value, 10))}
        />
        <span
          data-testid="image-literal-brightness-preview"
          data-level={level.toString(16)}
          className="flex items-center justify-center rounded-md bg-panel"
          style={wellStyle(kEditorWellSize)}
        >
          <span style={ledStyle(imageLevelBrightness(level), kEditorLed)} />
        </span>
      </div>
    </div>
  );
}

/** The lit grid a placed image literal draws in its value box. */
function ImagePreview({ bytes }: { bytes: readonly number[] }) {
  return (
    <div
      data-testid="image-literal-preview"
      className="grid grid-cols-5 rounded-sm bg-panel"
      style={{ gap: `${kPreviewWellGap}px`, padding: `${kPreviewGridPadding}px` }}
    >
      {bytes.map((brightness, index) => (
        <span key={cellKey(index)} className="flex items-center justify-center" style={wellStyle(kPreviewWellSize)}>
          <span
            data-testid="image-literal-pixel"
            data-brightness={brightness}
            style={ledStyle(brightness, kPreviewLed)}
          />
        </span>
      ))}
    </div>
  );
}

/**
 * The brain editor's custom literal type for `Image` values: a 5x5 grid editor
 * in the create-literal dialog, and a lit preview of the drawn pixels in placed
 * literal tiles.
 */
export const imageLiteralType: CustomLiteralType = {
  typeId: WODAL_SHARED_TYPE_IDS.Image,
  description: "Draw an image for the display. Pick a brightness, then tap the pixels to light.",
  nameBase: "image",

  isValid(state: Record<string, string>): boolean {
    return imageGridDigits(state) !== undefined;
  },

  parseValue(state: Record<string, string>): unknown {
    const digits = imageGridDigits(state);
    return digits === undefined ? undefined : imageValueFromDigits(digits);
  },

  toInputState(value: unknown): Record<string, string> {
    const digits = imageDigitsFromValue(value);
    return digits === undefined ? {} : { [kImagePixelsFieldKey]: digits };
  },

  renderInputFields(state: Record<string, string>, onChange: (key: string, value: string) => void) {
    return (
      <ImageGridEditor
        digits={imageGridDigits(state) ?? kBlankGridDigits}
        onChange={(digits) => onChange(kImagePixelsFieldKey, digits)}
      />
    );
  },

  formatValue(): string {
    return "image";
  },

  renderValue(value: unknown) {
    const bytes = imageGridBytes(value);
    return bytes === undefined ? undefined : <ImagePreview bytes={bytes} />;
  },
};
