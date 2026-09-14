import { AssistantProvider } from "@wendoo/assistant-panel";
import { DocsSidebar, DocsSidebarProvider } from "@wendoo/docs";
import { useMemo, useSyncExternalStore } from "react";
import { Toaster } from "sonner";
import { createMicrobitTileVisualResolver, microbitDataTypeIcons, microbitDataTypeNames } from "./brain/editor-config";
import { BrainList } from "./components/BrainList";
import { BridgePanel } from "./components/BridgePanel";
import { ProjectHeader } from "./components/ProjectHeader";
import { useResolutionWarningsToast } from "./components/ResolutionWarningsToast";
import { Simulator } from "./components/Simulator";
import { useMicrobitSimEnvironment } from "./contexts/microbit-sim-environment";
import { createDocsTileCatalog, createMicrobitDocsRegistry } from "./docs/docs-registry";

/** Root application component for the Wendoo micro:bit target. */
export function App() {
  const store = useMicrobitSimEnvironment();
  const chrome = store.chrome;
  useResolutionWarningsToast();
  // Rebuild the registry when user tiles install so their doc entries stay current,
  // and the visual resolver when the VFS revision advances so tile icons re-resolve.
  const docRevision = useSyncExternalStore(store.subscribeToDocRevision, store.getDocRevisionSnapshot);
  const vfsRevision = useSyncExternalStore(store.subscribeToVfsRevision, store.getVfsRevisionSnapshot);
  const docsRegistry = useMemo(() => {
    void docRevision;
    return createMicrobitDocsRegistry(store.env, store.host.lastUserTileMetadata);
  }, [docRevision, store]);
  const docsLibraries = useMemo(() => {
    void docRevision;
    return store.host.installedLibraries;
  }, [docRevision, store]);
  const docsTileCatalog = useMemo(() => createDocsTileCatalog(store.env), [store]);
  const docsResolveTileVisual = useMemo(() => {
    void vfsRevision;
    return createMicrobitTileVisualResolver((url) => store.resolveVfsAssetUrl(url));
  }, [store, vfsRevision]);
  return (
    <AssistantProvider
      connect={store.assistant.connect}
      manifest={store.assistant.manifest}
      clientBuild={store.assistant.clientBuild}
      workspace={store.assistant.workspaces.workspaceFor}
      activity={store.assistant.activity}
    >
      <DocsSidebarProvider
        registry={docsRegistry}
        tileCatalog={docsTileCatalog}
        brainServices={store.env.brainServices}
        libraries={docsLibraries}
        dataTypeNames={microbitDataTypeNames}
        dataTypeIcons={microbitDataTypeIcons}
        showDocsPageLinks={chrome.showDocsPageLinks}
        printTransport={store.printTransport}
        resolveTileVisual={docsResolveTileVisual}
      >
        <div className="min-h-screen">
          <ProjectHeader />
          <main className="p-4 sm:p-6">
            <div className="mx-auto grid w-full max-w-7xl grid-cols-1 items-start gap-8 lg:grid-cols-2">
              {/* On a single column the wrapper dissolves (display: contents), making all three
                  sections grid items so the bridge panel can order below the simulator. */}
              <div className="contents lg:block lg:space-y-8">
                <BrainList />
                {chrome.showBridgePanel && (
                  <div className="order-last">
                    <BridgePanel />
                  </div>
                )}
              </div>
              <Simulator />
            </div>
          </main>
          <Toaster />
        </div>
        {/* Docs sidebar -- fixed overlay, sibling to the main layout */}
        <DocsSidebar />
      </DocsSidebarProvider>
    </AssistantProvider>
  );
}
