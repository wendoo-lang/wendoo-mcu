import { AccelerometerGesture } from "@wendoo/wodal/targets/microbit-v2";
import { useEffect, useState } from "react";
import type { SimulatorInstance } from "@/services/simulator";

/** The selectable gestures, in display order: a resting `none` plus the eight shipped gestures. */
const GESTURE_OPTIONS: readonly { code: AccelerometerGesture; label: string }[] = [
  { code: AccelerometerGesture.None, label: "none" },
  { code: AccelerometerGesture.Shake, label: "shake" },
  { code: AccelerometerGesture.TiltLeft, label: "tilt left" },
  { code: AccelerometerGesture.TiltRight, label: "tilt right" },
  { code: AccelerometerGesture.TiltUp, label: "tilt up" },
  { code: AccelerometerGesture.TiltDown, label: "tilt down" },
  { code: AccelerometerGesture.FaceUp, label: "face up" },
  { code: AccelerometerGesture.FaceDown, label: "face down" },
  { code: AccelerometerGesture.Freefall, label: "freefall" },
];

/**
 * Per-device gesture picker. Selecting a gesture drives the instance's gesture
 * injector, which feeds the real wodal detector. Postures hold until changed;
 * shake and freefall play once and reset the picker back to `none`. Every
 * selection also fires `onSelected`, after the injector has taken the gesture.
 */
export function GesturePicker({ instance, onSelected }: { instance: SimulatorInstance; onSelected: () => void }) {
  // Seed from the injector so a remount (reopening the Popover) recovers a held posture.
  const [selected, setSelected] = useState<AccelerometerGesture>(() => instance.gestureInjector.currentGesture());

  useEffect(() => {
    instance.gestureInjector.setCompletionListener(() => setSelected(AccelerometerGesture.None));
    return () => instance.gestureInjector.setCompletionListener(undefined);
  }, [instance]);

  const onSelect = (code: AccelerometerGesture) => {
    setSelected(code);
    instance.gestureInjector.select(code);
    onSelected();
  };

  return (
    <label className="flex items-center gap-2 text-xs text-muted-foreground">
      Gesture
      <select
        data-testid="gesture-select"
        className="rounded border bg-background px-2 py-1 text-sm text-foreground"
        value={selected}
        onChange={(event) => onSelect(Number(event.target.value) as AccelerometerGesture)}
      >
        {GESTURE_OPTIONS.map((option) => (
          <option key={option.code} value={option.code}>
            {option.label}
          </option>
        ))}
      </select>
    </label>
  );
}
