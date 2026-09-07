import { useState, useSyncExternalStore } from "react";
import { useMicrobitSimEnvironment } from "@/contexts/microbit-sim-environment";
import { deviceLedGlow, ledColor } from "@/led-palette";
import type { SimulatorInstance } from "@/services/simulator";

/** Renders one simulated microbit: its live 5x5 display and the A, B, A+B, and Logo input buttons. */
export function MicrobitDevice({ instance }: { instance: SimulatorInstance }) {
  const store = useMicrobitSimEnvironment();
  // Re-render each frame so the display reflects the running program.
  useSyncExternalStore(store.simulator.subscribeToFrame, store.simulator.getFrame);
  const display = instance.snapshot().display;

  return (
    <div className="flex items-center gap-2">
      <div className="flex flex-col gap-2">
        <DeviceButton label="A" onChange={(pressed) => instance.microbit.setButtonPressed("A", pressed)} />
        <DeviceButton label="Logo" onChange={(touched) => instance.microbit.setLogoTouched(touched)} />
      </div>
      <div
        data-testid="display"
        role="img"
        aria-label="LED display"
        className="grid grid-cols-5 gap-1 rounded bg-panel p-2"
      >
        {display.pixels.map((brightness, i) => {
          const cellKey = `r${Math.trunc(i / display.width)}c${i % display.width}`;
          return (
            <div
              key={cellKey}
              data-testid="led"
              data-brightness={brightness}
              className="h-4 w-4 rounded-sm"
              style={{ background: ledColor(brightness), boxShadow: deviceLedGlow(brightness) }}
            />
          );
        })}
      </div>
      <div className="flex flex-col gap-2">
        <DeviceButton label="B" onChange={(pressed) => instance.microbit.setButtonPressed("B", pressed)} />
        <DeviceButton
          label="A+B"
          onChange={(pressed) => {
            instance.microbit.setButtonPressed("A", pressed);
            instance.microbit.setButtonPressed("B", pressed);
          }}
        />
      </div>
    </div>
  );
}

function DeviceButton({ label, onChange }: { label: string; onChange: (pressed: boolean) => void }) {
  const [pressed, setPressed] = useState(false);
  const set = (next: boolean) => {
    setPressed(next);
    onChange(next);
  };
  return (
    <button
      type="button"
      data-testid={`device-button-${label.toLowerCase()}`}
      className={`h-9 w-9 shrink-0 rounded-full border text-sm font-bold ${
        pressed ? "bg-primary text-primary-foreground" : "bg-muted text-muted-foreground"
      }`}
      onPointerDown={() => set(true)}
      onPointerUp={() => set(false)}
      onPointerLeave={() => set(false)}
      onKeyDown={(event) => {
        if ((event.key === " " || event.key === "Enter") && !event.repeat) {
          set(true);
        }
      }}
      onKeyUp={(event) => {
        if (event.key === " " || event.key === "Enter") {
          set(false);
        }
      }}
      onBlur={() => set(false)}
    >
      {label}
    </button>
  );
}
