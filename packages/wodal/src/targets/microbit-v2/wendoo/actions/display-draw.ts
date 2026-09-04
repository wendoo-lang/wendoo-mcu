import {
  type AsyncHandle,
  bag,
  type CreateHostActuatorOptions,
  type ExecutionContext,
  extractNumberValue,
  getSlotId,
  isListValue,
  isStructValue,
  mkCallDef,
  mkNumberValue,
  optional,
  type ReadonlyList,
  repeated,
  type Value,
  VOID_VALUE,
} from "@wendoo/core/app";
import type { BrainActionCallArgSpec } from "@wendoo/core/runtime";
import { bufferByteAt, bufferLength, isBufferValue } from "@wendoo/core/runtime";
import { toNonNegativeInteger } from "../../../../core/numeric";
import { ImageField } from "../../../../wendoo/shared-type-ids";
import { MICROBIT_LED_MATRIX_SIZE } from "../../constants";
import { builtInImageFrame, DEFAULT_BUILT_IN_IMAGE_NAME, getBuiltInImage } from "../built-in-images";
import { getMicroBitContextDevice, reportDeviceOperationEnding } from "../context";
import { hasModifier, Modifier } from "../modifiers";
import { Param } from "../parameters";
import { MicroBitV2HostActions } from "../tile-ids";

const MS_PER_SECOND = 1000;

/** Milliseconds a draw holds the display when the call omits the optional duration. */
export const DEFAULT_DURATION_MS = 1000;

/** The built-in image drawn when the call omits the optional image (the `happy` icon). */
export const DEFAULT_IMAGE: ClippedFrame = builtInImageFrame(getBuiltInImage(DEFAULT_BUILT_IN_IMAGE_NAME));

/** How long the drawing stands, in seconds; a negative one shows nothing. */
const Duration = {
  ...Param.duration,
  default: mkNumberValue(DEFAULT_DURATION_MS / MS_PER_SECOND),
  range: { min: 0, onExceed: "clamp" },
} satisfies BrainActionCallArgSpec;

const callDef = mkCallDef(
  bag(
    optional(repeated(Param.image, { min: 0 })),
    optional(Duration),
    optional(Modifier.immediately),
    optional(Modifier.inBackground)
  )
);

const kImageSlotId = getSlotId(callDef, Param.image);
const kDurationSlotId = getSlotId(callDef, Duration);
const kImmediatelySlotId = getSlotId(callDef, Modifier.immediately);
const kInBackgroundSlotId = getSlotId(callDef, Modifier.inBackground);

/** A draw frame clipped to the display: packed brightness bytes plus its clipped size. */
export interface ClippedFrame {
  /** Brightness bytes, row-major, length `width * height`. */
  readonly frame: number[];

  /** Clipped width in columns, at most the display width. */
  readonly width: number;

  /** Clipped height in rows, at most the display height. */
  readonly height: number;
}

/**
 * Clips an `Image` struct value to the display, returning the top-left region as
 * a packed brightness frame, or undefined when the value is not an `Image` (a
 * struct with numeric `width`/`height` and a `pixels` buffer). Pixels are read
 * from the buffer at the image's own row stride; cells past the buffer's end
 * read as brightness 0.
 */
export function clipImage(value: Value): ClippedFrame | undefined {
  if (!isStructValue(value) || value.v === undefined) {
    return undefined;
  }
  const widthValue = extractNumberValue(value.v.at(ImageField.Width));
  const heightValue = extractNumberValue(value.v.at(ImageField.Height));
  const pixels = value.v.at(ImageField.Pixels);
  if (widthValue === undefined || heightValue === undefined || !isBufferValue(pixels)) {
    return undefined;
  }
  const imageWidth = toNonNegativeInteger(widthValue);
  const imageHeight = toNonNegativeInteger(heightValue);
  const width = Math.min(imageWidth, MICROBIT_LED_MATRIX_SIZE);
  const height = Math.min(imageHeight, MICROBIT_LED_MATRIX_SIZE);
  const pixelCount = bufferLength(pixels);
  const frame: number[] = [];
  for (let row = 0; row < height; row++) {
    for (let col = 0; col < width; col++) {
      const index = row * imageWidth + col;
      frame.push(index < pixelCount ? (bufferByteAt(pixels, index) ?? 0) : 0);
    }
  }
  return { frame, width, height };
}

/**
 * Clips the image slot value into an ordered sequence of display frames. The
 * slot is a `List<Image>` (the repeated anonymous image arg): each clippable
 * element becomes a frame, in order. An empty or nil slot, or one with no
 * clippable element, yields the single default image.
 */
function clipImageSequence(value: Value | undefined): ClippedFrame[] {
  const frames: ClippedFrame[] = [];
  if (isListValue(value)) {
    for (let i = 0; i < value.v.size(); i++) {
      const clipped = clipImage(value.v.get(i));
      if (clipped !== undefined) {
        frames.push(clipped);
      }
    }
  } else if (value !== undefined) {
    const clipped = clipImage(value);
    if (clipped !== undefined) {
      frames.push(clipped);
    }
  }
  if (frames.length === 0) {
    frames.push(DEFAULT_IMAGE);
  }
  return frames;
}

function execDrawImage(ctx: ExecutionContext, args: ReadonlyList<Value>, handle: AsyncHandle): void {
  const microbit = getMicroBitContextDevice(ctx);
  if (!microbit) {
    handle.resolve(VOID_VALUE);
    return;
  }
  if (hasModifier(args, kImmediatelySlotId)) {
    microbit.display.preempt();
  }
  const frames = clipImageSequence(args.at(kImageSlotId));
  const durationSeconds = extractNumberValue(args.at(kDurationSlotId));
  // Convert the seconds argument to whole ms at f32 precision, matching the device.
  const durationMs =
    durationSeconds === undefined
      ? DEFAULT_DURATION_MS
      : toNonNegativeInteger(Math.fround(durationSeconds * MS_PER_SECOND));
  const inBackground = hasModifier(args, kInBackgroundSlotId);
  microbit.display.drawImage(frames, durationMs, ctx.time, (end) => {
    handle.resolve(VOID_VALUE);
    reportDeviceOperationEnding(ctx, { handleId: handle.id, end, inBackground });
  });
  if (inBackground) {
    // The draw keeps its lease and resolves on tick time as above; resolving now
    // releases the issuing rule so it does not park on the hold.
    handle.resolve(VOID_VALUE);
  }
}

/**
 * Host actuator: paste one or more `Image`s to the simulated display top-left,
 * clipped to the 5x5 matrix. Args: the repeated anonymous `Image` slot (a
 * `List<Image>`; when empty or nil a default smiley is drawn) and the named hold
 * duration in seconds (when absent 1 second). With more than one image the
 * images play in sequence, each held for the duration, under one lease (total =
 * imageCount x duration). Asynchronous -- an explicit zero-duration draw resolves
 * at dispatch (fire-and-forget, no lease; with multiple images only the last is
 * painted); a positive-duration draw (including the default 1 second) holds the
 * display lease for the whole sequence and resolves when it elapses, with the
 * awaiting fiber parked until then. With the `immediately` modifier the current
 * display lease is preempted so the draw runs at once; otherwise a draw
 * dispatched while the display is busy is silently dropped. With the
 * `in background` modifier the draw keeps its lease but the handle resolves at
 * dispatch, so the issuing rule continues this round without parking on the hold.
 */
export default {
  ...MicroBitV2HostActions.DrawImage,
  callDef,
  fn: { exec: execDrawImage },
  isAsync: true,
  metadata: {
    label: "draw image",
  },
} satisfies CreateHostActuatorOptions;
