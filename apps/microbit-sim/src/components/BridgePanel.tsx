import { useDocsSidebar } from "@wendoo/docs";
import { Switch } from "@wendoo/ui";
import { CircleHelp } from "lucide-react";
import { useEffect, useId, useState, useSyncExternalStore } from "react";
import { useMicrobitSimEnvironment } from "@/contexts/microbit-sim-environment";
import { clearBindingToken } from "@/services/binding-token-persistence";
import { BridgeConnectionStatus } from "./BridgeConnectionStatus";
import { CompileDiagnosticsConsole } from "./CompileDiagnosticsConsole";

/**
 * Developer panel: the VS Code bridge section (an enable toggle, the
 * connection readout -- status, a copyable join code, and the notice of a
 * failure holding the connection down with its way back -- and help
 * affordances that open the docs sidebar to the "Connect VS Code" concept
 * page) and the build-output section (the latest workspace compile's
 * diagnostics as console output, present only when the compile carries
 * diagnostics). The
 * enable toggle drives the per-project `bridgeEnabled` preference; an
 * enabled bridge connects on mount and whenever the toggle flips on, and
 * turning it off ends the session and forgets its binding token.
 */
export function BridgePanel() {
  const store = useMicrobitSimEnvironment();
  const bridgeStatus = useSyncExternalStore(store.subscribeToBridgeStatus, store.getBridgeStatusSnapshot);
  const joinCode = useSyncExternalStore(store.subscribeToBridgeJoinCode, store.getBridgeJoinCodeSnapshot);
  const bridgePaired = useSyncExternalStore(store.subscribeToBridgePaired, store.getBridgePairedSnapshot);
  const bridgeErrorCode = useSyncExternalStore(store.subscribeToBridgeErrorCode, store.getBridgeErrorCodeSnapshot);
  const compileDiagnostics = useSyncExternalStore(
    store.subscribeToCompileDiagnostics,
    store.getCompileDiagnosticsSnapshot
  );
  const [bridgeEnabled, setBridgeEnabled] = useState(() => store.getUiPreferences().bridgeEnabled);
  const headingId = useId();
  const { open: openDocs, navigateToEntry } = useDocsSidebar();

  const showVsCodeHelp = () => {
    openDocs();
    navigateToEntry("concepts", "vscode");
  };

  useEffect(() => {
    return store.onProjectLoaded(() => {
      setBridgeEnabled(store.getUiPreferences().bridgeEnabled);
    });
  }, [store]);

  useEffect(() => {
    if (bridgeEnabled) {
      store.connectBridge();
    }
  }, [bridgeEnabled, store]);

  return (
    <section aria-labelledby={headingId} className="max-w-md">
      <h2 id={headingId} className="text-base font-bold">
        Dev Panel
      </h2>
      <div className="mt-3 space-y-2 rounded-lg border p-3">
        <div className="flex flex-wrap items-center justify-between gap-2">
          <div className="flex items-center gap-0.5">
            <h3 className="text-sm font-medium">VS Code Bridge</h3>
            <button
              type="button"
              className="shrink-0 rounded p-0.5 text-muted-foreground transition-colors hover:text-foreground"
              aria-label="VS Code Bridge Help"
              onClick={showVsCodeHelp}
            >
              <CircleHelp className="h-3.5 w-3.5" aria-hidden="true" />
            </button>
          </div>
          <Switch
            checked={bridgeEnabled}
            onCheckedChange={(checked) => {
              setBridgeEnabled(checked);
              store.updateUiPreferences({ bridgeEnabled: checked });
              if (!checked) {
                store.endBridge();
                clearBindingToken();
              }
            }}
            aria-label="Toggle VS Code bridge connection"
          />
        </div>
        <BridgeConnectionStatus
          status={bridgeStatus}
          paired={bridgePaired}
          errorCode={bridgeErrorCode}
          joinCode={joinCode}
          onReconnect={() => {
            store.connectBridge();
          }}
        />
        <button
          type="button"
          className="text-left text-xs text-muted-foreground underline-offset-2 transition-colors hover:text-foreground hover:underline"
          onClick={showVsCodeHelp}
        >
          How to connect VS Code
        </button>
      </div>
      {compileDiagnostics.length > 0 && (
        <div className="mt-3 space-y-2 rounded-lg border p-3">
          <h3 className="text-sm font-medium">Build issues</h3>
          <CompileDiagnosticsConsole diagnostics={compileDiagnostics} />
        </div>
      )}
    </section>
  );
}
