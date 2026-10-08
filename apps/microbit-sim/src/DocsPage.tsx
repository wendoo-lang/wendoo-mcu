import { DocsPage as SharedDocsPage } from "@wendoo/docs";
import { Toaster } from "@wendoo/ui";
import { useMemo, useSyncExternalStore } from "react";
import { createMicrobitTileVisualResolver, microbitDataTypeIcons, microbitDataTypeNames } from "./brain/editor-config";
import { useMicrobitSimEnvironment } from "./contexts/microbit-sim-environment";
import { createDocsTileCatalog, createMicrobitDocsRegistry } from "./docs/docs-registry";

/** Full-page documentation view served at `/docs`, with the toast region the docs announce through. */
export function DocsPage() {
  const store = useMicrobitSimEnvironment();
  // Rebuild the registry and library list when user tiles install so a library added while this page
  // is open surfaces its tiles and docs, and the visual resolver when the VFS revision advances so
  // tile icons re-resolve against the new asset generation.
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
  const resolveTileVisual = useMemo(() => {
    void vfsRevision;
    return createMicrobitTileVisualResolver((url) => store.resolveVfsAssetUrl(url));
  }, [store, vfsRevision]);

  return (
    <SharedDocsPage
      registry={docsRegistry}
      tileCatalog={docsTileCatalog}
      brainServices={store.env.brainServices}
      libraries={docsLibraries}
      dataTypeNames={microbitDataTypeNames}
      dataTypeIcons={microbitDataTypeIcons}
      resolveTileVisual={resolveTileVisual}
      backLabel="Simulator"
      backHref="/"
    >
      <Toaster />
    </SharedDocsPage>
  );
}
