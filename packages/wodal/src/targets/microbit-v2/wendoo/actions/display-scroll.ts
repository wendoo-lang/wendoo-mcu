import {
  type AsyncHandle,
  bag,
  type CreateHostActuatorOptions,
  type ExecutionContext,
  extractNumberValue,
  extractStringValue,
  formatF32,
  getSlotId,
  getWhenResult,
  isNilValue,
  mkCallDef,
  optional,
  type ReadonlyList,
  type Value,
  VOID_VALUE,
} from "@wendoo/core/app";
import { getMicroBitContextDevice, reportDeviceOperationEnding } from "../context";
import { SCROLL_DEFAULT_DELAY_MS, scrollDurationMs } from "../display-scroll";
import { hasModifier, Modifier } from "../modifiers";
import { Param } from "../parameters";
import { MicroBitV2HostActions } from "../tile-ids";

/** Text shown when the call omits the optional text argument. */
const DEFAULT_TEXT = "hello";

const callDef = mkCallDef(bag(optional(Param.text), optional(Modifier.immediately), optional(Modifier.inBackground)));

const kTextSlotId = getSlotId(callDef, Param.text);
const kImmediatelySlotId = getSlotId(callDef, Modifier.immediately);
const kInBackgroundSlotId = getSlotId(callDef, Modifier.inBackground);

/** True when arg slot `slotId` carries a present (non-nil) value. */
function hasArg(args: ReadonlyList<Value>, slotId: number): boolean {
  const value = args.at(slotId);
  return value !== undefined && !isNilValue(value);
}

/**
 * Best-effort text for the rule's captured WHEN result: a number renders through
 * the binary32 formatter and a string passes through; any other value (a
 * boolean, nil, or container) yields undefined so the caller keeps its default.
 */
function whenResultText(ctx: ExecutionContext): string | undefined {
  const whenResult = getWhenResult(ctx);
  const num = extractNumberValue(whenResult);
  if (num !== undefined) {
    return formatF32(num);
  }
  return extractStringValue(whenResult);
}

function execDisplayScroll(ctx: ExecutionContext, args: ReadonlyList<Value>, handle: AsyncHandle): void {
  const text = hasArg(args, kTextSlotId)
    ? (extractStringValue(args.at(kTextSlotId)) ?? DEFAULT_TEXT)
    : (whenResultText(ctx) ?? DEFAULT_TEXT);
  const microbit = getMicroBitContextDevice(ctx);
  if (!microbit) {
    handle.resolve(VOID_VALUE);
    return;
  }
  if (hasModifier(args, kImmediatelySlotId)) {
    microbit.display.preempt();
  }
  const inBackground = hasModifier(args, kInBackgroundSlotId);
  const durationMs = scrollDurationMs(text.length, SCROLL_DEFAULT_DELAY_MS);
  microbit.display.scrollText(text, durationMs, ctx.time, (end) => {
    handle.resolve(VOID_VALUE);
    reportDeviceOperationEnding(ctx, { handleId: handle.id, end, inBackground });
  });
  if (inBackground) {
    // The scroll keeps its lease and resolves on tick time as above; resolving
    // now releases the issuing rule so it does not park on the animation.
    handle.resolve(VOID_VALUE);
  }
}

/**
 * Host actuator: show text on the simulated display - a scroll for a text of
 * any length other than one, a static glyph show for a one-character text
 * (held for the duration the scroll formula gives it, then blanked).
 * Asynchronous -- the calling fiber awaits the returned handle and resumes when
 * the show completes. With the `immediately` modifier the current display lease
 * is preempted so the show starts at once; otherwise a show requested while
 * the display is busy is dropped. With the `in background` modifier the show
 * keeps its lease but the handle resolves at dispatch, so the issuing rule
 * continues this round without parking on the animation.
 */
export default {
  ...MicroBitV2HostActions.DisplayScroll,
  callDef,
  fn: { exec: execDisplayScroll },
  isAsync: true,
  metadata: {
    label: "display text",
    grammarNote:
      'The rule holds until the text has finished showing: until then a rule under it does not get its turn, and this rule cannot fire again. A show asked for while the display is busy is dropped; add "in background" to let the rule carry on, or "immediately" to cut off what the display is showing.',
  },
} satisfies CreateHostActuatorOptions;
