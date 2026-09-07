import { Popover, PopoverContent, PopoverTrigger } from "@wendoo/ui";
import { MoreHorizontal, Pencil } from "lucide-react";
import { useState, useSyncExternalStore } from "react";
import { useMicrobitSimEnvironment } from "@/contexts/microbit-sim-environment";
import type { BrainRecord } from "@/services/microbit-sim-environment-store";
import type { SimulatorInstance } from "@/services/simulator";
import { BrainEditor } from "./BrainEditor";
import { GesturePicker } from "./GesturePicker";
import { kIconTriggerClasses } from "./icon-trigger";
import { LightLevelSlider } from "./LightLevelSlider";
import { MicrobitDevice } from "./MicrobitDevice";
import { TemperatureSlider } from "./TemperatureSlider";

interface InstanceCardProps {
  instance: SimulatorInstance;
  label: string;
  brains: readonly BrainRecord[];
}

/**
 * The state-dependent class for the brain selector: red destructive tokens when
 * the assigned brain has errors, the plain foreground token when a clean brain
 * is assigned, and the amber warning tokens when no brain is assigned yet.
 */
export function brainSelectStateClass(assigned: boolean, hasErrors: boolean): string {
  if (!assigned) {
    return "border-warning text-warning";
  }
  return hasErrors ? "border-destructive text-destructive" : "text-foreground";
}

/**
 * One simulator instance: a brain picker that flashes the picked brain onto the
 * device, the device itself, and a device-inputs and actions panel behind the
 * kebab. The panel holds the synthetic device inputs (gesture, light level) and
 * the Reset / Remove actions.
 */
export function InstanceCard({ instance, label, brains }: InstanceCardProps) {
  const store = useMicrobitSimEnvironment();
  const [editingBrain, setEditingBrain] = useState(false);
  const [actionsOpen, setActionsOpen] = useState(false);
  // Re-render when a brain's diagnostics change (a broken tile fixed, a build failing)
  // so the selector's error color tracks the same signal as the brain-list badge.
  useSyncExternalStore(store.subscribeToBrainDiagnostics, store.getBrainDiagnosticsRevision);
  const assigned = instance.flashedBrainId != null;
  const hasErrors = instance.flashedBrainId != null && store.brainHasErrors(instance.flashedBrainId);

  return (
    // Card width hugs the device row: two button columns + the 112px display + two 8px gaps, plus
    // the card's own padding and border. The buttons are 36px wide on a fine pointer and 44px under
    // the shared coarse-pointer floor, so the card carries a width for each.
    <div
      data-testid="instance-card"
      className="flex w-58.5 max-w-full flex-col gap-3 rounded-lg border p-4 pointer-coarse:w-62.5"
    >
      <div className="flex items-start justify-between gap-2">
        <div className="flex min-w-0 flex-1 flex-col gap-1">
          <select
            data-testid="instance-brain-select"
            aria-label={`Brain for ${label}`}
            disabled={brains.length === 0}
            className={`w-full rounded border bg-background px-2 py-1 text-sm disabled:opacity-50 ${brainSelectStateClass(assigned, hasErrors)}`}
            value={instance.flashedBrainId ?? ""}
            onChange={(event) => {
              if (event.target.value) {
                void store.flashBrainToInstance(instance.id, event.target.value);
              }
            }}
          >
            <option value="" disabled hidden>
              (no brain)
            </option>
            {brains.map((brain) => (
              <option key={brain.id} value={brain.id}>
                {brain.name}
              </option>
            ))}
          </select>
        </div>
        <div className="flex shrink-0 items-center gap-1">
          <button
            type="button"
            data-testid="instance-edit-brain"
            aria-label={`Edit brain for ${label}`}
            disabled={!assigned}
            className={`${kIconTriggerClasses} disabled:pointer-events-none disabled:opacity-50`}
            onClick={() => setEditingBrain(true)}
          >
            <Pencil className="h-4 w-4" />
          </button>
          <Popover open={actionsOpen} onOpenChange={setActionsOpen}>
            <PopoverTrigger asChild>
              <button
                type="button"
                data-testid="instance-actions"
                className={kIconTriggerClasses}
                aria-label={`More actions for ${label}`}
              >
                <MoreHorizontal className="h-4 w-4" />
              </button>
            </PopoverTrigger>
            <PopoverContent align="end" className="w-56">
              <div className="flex flex-col gap-3">
                <div className="flex flex-col gap-2">
                  <p className="text-xs font-semibold text-muted-foreground">Inputs</p>
                  <GesturePicker instance={instance} onSelected={() => setActionsOpen(false)} />
                  <LightLevelSlider instance={instance} />
                  <TemperatureSlider instance={instance} />
                </div>
                <div className="-mx-4 h-px bg-muted" />
                <div className="flex flex-col gap-1">
                  <button
                    type="button"
                    data-testid="instance-reset"
                    disabled={!assigned}
                    className="rounded px-2 py-1.5 text-left text-sm hover:bg-accent hover:text-accent-foreground disabled:pointer-events-none disabled:opacity-50"
                    onClick={() => {
                      setActionsOpen(false);
                      void store.resetInstance(instance.id);
                    }}
                  >
                    Reset Device
                  </button>
                  <button
                    type="button"
                    data-testid="instance-remove"
                    className="rounded px-2 py-1.5 text-left text-sm text-destructive hover:bg-accent hover:text-destructive"
                    onClick={() => store.simulator.removeInstance(instance.id)}
                  >
                    Remove Device
                  </button>
                </div>
              </div>
            </PopoverContent>
          </Popover>
        </div>
      </div>

      <MicrobitDevice instance={instance} />

      {editingBrain && instance.flashedBrainId && (
        <BrainEditor brainId={instance.flashedBrainId} onClose={() => setEditingBrain(false)} />
      )}
    </div>
  );
}
