import { toast } from "@wendoo/ui";
import { useEffect } from "react";
import { useMicrobitSimEnvironment } from "../contexts/microbit-sim-environment";
import type { ResolutionWarningsToastPresenter, UnresolvedLibrariesToast } from "../services/resolution-warnings-toast";
import { startResolutionWarningsToast } from "../services/resolution-warnings-toast";

/** Toast description naming each displayed coordinate plus any overflow count. */
function UnresolvedLibrariesDescription({
  coordinates,
  overflowCount,
}: Pick<UnresolvedLibrariesToast, "coordinates" | "overflowCount">) {
  return (
    <span>
      <span className="font-mono">{coordinates.join(" ")}</span>
      {overflowCount > 0 ? ` +${overflowCount} more` : null}
    </span>
  );
}

/** Presents the unresolved-libraries warning through the app's sonner toaster. */
const sonnerPresenter: ResolutionWarningsToastPresenter = {
  showWarning(warningToast) {
    toast.warning("Some libraries could not be loaded", {
      id: warningToast.id,
      duration: warningToast.duration,
      closeButton: warningToast.closeButton,
      description: (
        <UnresolvedLibrariesDescription
          coordinates={warningToast.coordinates}
          overflowCount={warningToast.overflowCount}
        />
      ),
      onDismiss: warningToast.onDismiss,
    });
  },
  dismiss(id) {
    toast.dismiss(id);
  },
};

/**
 * Shows a persistent warning toast while the active project has libraries
 * whose content failed to resolve on the last load or install. The toast
 * updates in place as the unresolved set changes and clears on its own once
 * a later load or install resolves them.
 */
export function useResolutionWarningsToast(): void {
  const store = useMicrobitSimEnvironment();
  useEffect(() => startResolutionWarningsToast(store, sonnerPresenter), [store]);
}
