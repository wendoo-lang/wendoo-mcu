import { parseExtensionReference, type UnstableDependency } from "@wendoo/app-host";
import type { ExtensionTransactionToasts, LibraryUninstallImpact } from "@wendoo/bridge-app";
import { presentExtensionTransaction, runGuardedLibraryUninstall } from "@wendoo/bridge-app";
import { useDocsSidebar } from "@wendoo/docs";
import {
  Button,
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuSeparator,
  DropdownMenuSub,
  DropdownMenuSubContent,
  DropdownMenuSubTrigger,
  DropdownMenuTrigger,
  ExtensionBrowserDialog,
} from "@wendoo/ui";
import {
  Blocks,
  BookOpen,
  ChevronDown,
  Download,
  ExternalLink,
  FilePlus,
  FolderOpen,
  Globe,
  Info,
  Menu,
  Settings,
  Upload,
} from "lucide-react";
import { useCallback, useMemo, useState, useSyncExternalStore } from "react";
import { toast } from "sonner";
import { useMicrobitSimEnvironment } from "@/contexts/microbit-sim-environment";
import { collectMicrobitLibraryUninstallImpact } from "@/services/library-uninstall-guard";
import { microbitEmbeddedExtensions } from "@/services/microbit-embedded-extensions";
import {
  buildMicrobitCatalogOffers,
  buildMicrobitExtensionEntries,
  checkMicrobitExtensionUpdates,
  installMicrobitExtension,
  installMicrobitReference,
  microbitCatalogCoordinateOrder,
  microbitLibraryDisplayName,
  uninstallMicrobitExtension,
} from "@/services/microbit-extension-browser";
import { downloadTextFile } from "@/utils/file-download";
import { pickFile } from "@/utils/file-upload";
import { ConfirmDialog } from "./ConfirmDialog";
import { GithubIcon } from "./GithubIcon";
import { InlineRename } from "./InlineRename";
import { NewProjectDialog } from "./NewProjectDialog";
import { ProjectPickerDialog } from "./ProjectPickerDialog";
import { SettingsDialog } from "./SettingsDialog";
import { UninstallImpactMessage } from "./UninstallImpactMessage";
import { WendooLogotype } from "./WendooLogo";

type OpenDialog = "none" | "new" | "open" | "settings" | "extensions";

/** An uninstall waiting on the in-use confirmation dialog. */
interface PendingUninstall {
  /** The library's display name; titles the dialog. */
  name: string;
  /** The computed in-use impact the dialog lists. */
  impact: LibraryUninstallImpact;
  /** Resolves the confirmation: true removes anyway, false cancels. */
  resolve: (confirmed: boolean) => void;
}

function failedToast(prefix: string): ExtensionTransactionToasts["failed"] {
  return ({ code, message }) => {
    toast.error(`${prefix} ${code !== undefined ? `${code}: ` : ""}${message}`);
  };
}

const installToasts: ExtensionTransactionToasts = {
  failed: failedToast("Could not install library."),
  confirmed: (name) => toast.success(`Installed ${name}`),
};

const uninstallToasts: ExtensionTransactionToasts = {
  failed: failedToast("Could not remove library."),
  confirmed: (name) => toast.success(`Removed ${name}`),
};

const refreshToasts: ExtensionTransactionToasts = {
  failed: failedToast("Could not load libraries."),
  confirmed: () => undefined,
};

function projectFilename(name: string): string {
  const slug = name.replace(/[^a-z0-9-_]+/gi, "-").replace(/^-+|-+$/g, "");
  return `${slug || "project"}.wendoo`;
}

/** Top bar showing the active project with create, open, import, and export controls. */
export function ProjectHeader() {
  const store = useMicrobitSimEnvironment();
  const chrome = store.chrome;
  const projectName = useSyncExternalStore(store.subscribeToActiveProject, store.getActiveProjectName);
  const getExtensions = useCallback(() => store.activeProjectManifest?.extensions, [store]);
  const extensions = useSyncExternalStore(store.subscribeToActiveProject, getExtensions);
  const [dialog, setDialog] = useState<OpenDialog>("none");
  const [pendingUninstall, setPendingUninstall] = useState<PendingUninstall | null>(null);
  const [unstableExportDependencies, setUnstableExportDependencies] = useState<readonly UnstableDependency[] | null>(
    null
  );
  const { toggle: toggleDocs, isOpen: isDocsOpen, open: openDocs, navigateToEntry } = useDocsSidebar();

  const catalogOffers = useMemo(() => buildMicrobitCatalogOffers(extensions, microbitEmbeddedExtensions), [extensions]);

  const extensionEntries = useMemo(
    () =>
      buildMicrobitExtensionEntries(
        extensions,
        microbitEmbeddedExtensions,
        store.host.installedExtensionContent,
        store.host.extensionFetchFailures
      ),
    [extensions, store]
  );

  const handleInstallExtension = async (coordinate: string) => {
    const result = await installMicrobitExtension(
      store.host,
      store.activeProjectManifest?.extensions,
      coordinate,
      microbitEmbeddedExtensions
    );
    if (!result.action.ok) {
      toast.error(`Could not install library (${result.action.code})`);
      return;
    }
    presentExtensionTransaction({
      report: result.report,
      flavor: "install",
      libraryName: microbitLibraryDisplayName(store.host.installedLibraries, coordinate),
      toasts: installToasts,
    });
  };

  const handleInstallExtensionReference = async (input: string) => {
    const result = await installMicrobitReference(
      store.host,
      store.activeProjectManifest?.extensions,
      microbitEmbeddedExtensions,
      input
    );
    if (!result.ok) {
      toast.error(`Could not add library. ${result.code}: ${result.message}`);
      return;
    }
    if (!result.action.ok) {
      toast.error(`Could not add library (${result.action.code})`);
      return;
    }
    const parsed = parseExtensionReference(result.reference);
    const coordinate =
      parsed === undefined
        ? result.reference
        : parsed.transport === "gh"
          ? `${parsed.owner}/${parsed.repo}`
          : parsed.coordinate;
    presentExtensionTransaction({
      report: result.report,
      flavor: "install",
      libraryName: microbitLibraryDisplayName(store.host.installedLibraries, coordinate),
      toasts: installToasts,
    });
  };

  const handleUninstallExtension = async (coordinate: string) => {
    const extensionsMap = store.activeProjectManifest?.extensions;
    const name = microbitLibraryDisplayName(store.host.installedLibraries, coordinate);
    const impact = collectMicrobitLibraryUninstallImpact(
      store.host,
      extensionsMap,
      coordinate,
      microbitEmbeddedExtensions,
      store.host.installedExtensionContent
    );
    await runGuardedLibraryUninstall({
      impact,
      confirmRemoval: () =>
        new Promise<boolean>((resolve) => {
          setPendingUninstall({ name, impact, resolve });
        }),
      uninstall: async () => {
        const result = await uninstallMicrobitExtension(
          store.host,
          extensionsMap,
          coordinate,
          microbitEmbeddedExtensions,
          store.host.installedExtensionContent
        );
        if (!result.action.ok) {
          toast.error(`Could not remove library (${result.action.code})`);
          return;
        }
        presentExtensionTransaction({
          report: result.report,
          flavor: "uninstall",
          libraryName: name,
          toasts: uninstallToasts,
        });
      },
    });
  };

  const handleCheckUpdates = async (coordinates: readonly string[]) => {
    const summary = await checkMicrobitExtensionUpdates(store.host, coordinates);
    for (const failure of summary.failures) {
      toast.error(`Could not check ${failure.coordinate} for updates. ${failure.error.code}: ${failure.error.message}`);
    }
    if (summary.updates.length === 0) {
      if (summary.failures.length === 0) {
        toast.success("Libraries are up to date");
      }
      return;
    }
    const updates = summary.updates;
    const description = updates
      .map((update) => `${update.coordinate} -> ${update.latestVersion ?? update.resolvedSha?.slice(0, 12) ?? ""}`)
      .join("\n");
    toast.info(`${updates.length} update(s) available`, {
      description,
      action: {
        label: updates.length > 1 ? "Update all" : "Update",
        onClick: () => {
          void (async () => {
            presentExtensionTransaction({
              report: await store.host.applyExtensionUpdates(updates),
              flavor: "refresh",
              toasts: refreshToasts,
            });
          })();
        },
      },
    });
  };

  const handleCheckAllUpdates = () =>
    handleCheckUpdates(extensionEntries.filter((entry) => entry.updatable === true).map((entry) => entry.coordinate));

  const handleRetryExtension = async () => {
    presentExtensionTransaction({
      report: await store.host.updateProjectExtensions(store.activeProjectManifest?.extensions ?? {}),
      flavor: "refresh",
      toasts: refreshToasts,
    });
  };

  const performExport = async () => {
    try {
      const content = await store.exportProject();
      downloadTextFile(content, projectFilename(store.getActiveProjectName()));
    } catch {
      toast.error("Export failed");
    }
  };

  const handleExport = async () => {
    const unstable = await store.host.collectUnstableProjectDependencies();
    if (unstable.length > 0) {
      setUnstableExportDependencies(unstable);
      return;
    }
    await performExport();
  };

  const handleImport = async () => {
    const file = await pickFile(".wendoo,.json");
    if (!file) {
      return;
    }
    const result = await store.importProject(file);
    if (!result.success) {
      const error = result.diagnostics.find((diagnostic) => diagnostic.severity === "error");
      toast.error(error?.message ?? "Import failed");
      return;
    }
    const warnings = result.diagnostics.filter((diagnostic) => diagnostic.severity === "warning");
    if (warnings.length > 0) {
      toast.warning(`Imported with ${warnings.length} warning(s)`, {
        description: warnings.map((warning) => warning.message).join("\n"),
      });
    } else {
      toast.success("Project imported");
    }
  };

  return (
    <header className="border-b border-border bg-header">
      <div
        aria-hidden="true"
        className="h-0.75 bg-linear-to-r from-(--strip-blue) via-(--strip-green) to-(--strip-teal)"
      />
      <div className="mx-auto flex w-full max-w-7xl flex-wrap items-center justify-between gap-y-2 px-4 py-3 sm:px-6">
        <div className="flex items-baseline gap-2">
          <h1 className="flex items-baseline gap-2 text-base font-bold">
            <span className="sr-only">Wendoo</span>
            <WendooLogotype className="h-5 w-auto translate-y-[10.5%]" />
            <span aria-hidden="true">/</span>
            <span>micro:bit</span>
          </h1>
          <span className="text-sm text-muted-foreground">/</span>
          <InlineRename value={projectName} ariaLabel="project name" onRename={(name) => store.renameProject(name)} />
        </div>
        <div className="flex flex-wrap gap-2">
          {chrome.showProjectMenu && (
            <DropdownMenu>
              <DropdownMenuTrigger asChild>
                <Button type="button" variant="outline" size="sm" data-testid="project-menu-button">
                  Project
                  <ChevronDown />
                </Button>
              </DropdownMenuTrigger>
              <DropdownMenuContent align="end">
                <DropdownMenuItem data-testid="new-project-button" onClick={() => setDialog("new")}>
                  <FilePlus />
                  New project...
                </DropdownMenuItem>
                <DropdownMenuItem data-testid="open-project-button" onClick={() => setDialog("open")}>
                  <FolderOpen />
                  Open...
                </DropdownMenuItem>
                <DropdownMenuSeparator />
                <DropdownMenuItem data-testid="import-project-button" onClick={() => void handleImport()}>
                  <Upload />
                  Import...
                </DropdownMenuItem>
                <DropdownMenuItem data-testid="export-project-button" onClick={() => void handleExport()}>
                  <Download />
                  Export
                </DropdownMenuItem>
                <DropdownMenuSeparator />
                <DropdownMenuItem data-testid="extensions-button" onClick={() => setDialog("extensions")}>
                  <Blocks />
                  Libraries...
                </DropdownMenuItem>
              </DropdownMenuContent>
            </DropdownMenu>
          )}
          {chrome.showExtensionsButton && (
            <Button
              type="button"
              variant="outline"
              size="sm"
              data-testid="host-extensions-button"
              onClick={() => setDialog("extensions")}
            >
              <Blocks />
              Libraries
            </Button>
          )}
          <Button
            type="button"
            variant="outline"
            size="sm"
            data-testid="docs-button"
            aria-label="Documentation"
            title="Documentation"
            aria-expanded={isDocsOpen}
            aria-controls="docs-sidebar"
            onClick={toggleDocs}
          >
            <BookOpen />
          </Button>
          <DropdownMenu>
            <DropdownMenuTrigger asChild>
              <Button type="button" variant="outline" size="sm" data-testid="app-menu-button" aria-label="Menu">
                <Menu />
              </Button>
            </DropdownMenuTrigger>
            <DropdownMenuContent align="end">
              {chrome.showSettings && (
                <>
                  <DropdownMenuItem data-testid="settings-button" onClick={() => setDialog("settings")}>
                    <Settings />
                    Settings
                  </DropdownMenuItem>
                  <DropdownMenuSeparator />
                </>
              )}
              <DropdownMenuItem
                data-testid="about-button"
                onClick={() => {
                  openDocs();
                  navigateToEntry("concepts", "about");
                }}
              >
                <Info />
                About
              </DropdownMenuItem>
              <DropdownMenuSub>
                <DropdownMenuSubTrigger>
                  <ExternalLink />
                  Learn more
                </DropdownMenuSubTrigger>
                <DropdownMenuSubContent>
                  <DropdownMenuItem asChild>
                    <a
                      data-testid="homepage-link"
                      href="https://wendoo-lang.org"
                      target="_blank"
                      rel="noopener noreferrer"
                    >
                      <Globe />
                      Wendoo Homepage
                    </a>
                  </DropdownMenuItem>
                  <DropdownMenuItem asChild>
                    <a
                      data-testid="github-link"
                      href="https://github.com/wendoo-lang/wendoo-lang"
                      target="_blank"
                      rel="noopener noreferrer"
                    >
                      <GithubIcon />
                      Wendoo Github
                    </a>
                  </DropdownMenuItem>
                </DropdownMenuSubContent>
              </DropdownMenuSub>
            </DropdownMenuContent>
          </DropdownMenu>
        </div>
      </div>
      {pendingUninstall !== null && (
        <ConfirmDialog
          title={pendingUninstall.name}
          message={<UninstallImpactMessage impact={pendingUninstall.impact} />}
          confirmLabel="Remove anyway"
          destructive
          onConfirm={async () => pendingUninstall.resolve(true)}
          onClose={() => {
            pendingUninstall.resolve(false);
            setPendingUninstall(null);
          }}
        />
      )}
      {dialog === "new" && <NewProjectDialog onClose={() => setDialog("none")} />}
      {dialog === "open" && <ProjectPickerDialog onClose={() => setDialog("none")} />}
      {dialog === "settings" && <SettingsDialog onClose={() => setDialog("none")} />}
      {unstableExportDependencies !== null && (
        <ConfirmDialog
          title="Export with unstable dependencies?"
          message={
            "This project depends on libraries that are not stable for consumers: " +
            `${unstableExportDependencies
              .map((dependency) => `${dependency.coordinate} (${dependency.code})`)
              .join(", ")}. ` +
            "The exported project may not work, or may not keep working, for anyone else."
          }
          confirmLabel="Export anyway"
          onConfirm={performExport}
          onClose={() => setUnstableExportDependencies(null)}
        />
      )}
      <ExtensionBrowserDialog
        open={dialog === "extensions"}
        onOpenChange={(open) => setDialog(open ? "extensions" : "none")}
        entries={extensionEntries}
        onInstall={handleInstallExtension}
        onUninstall={handleUninstallExtension}
        onCheckUpdate={(coordinate) => handleCheckUpdates([coordinate])}
        onRetry={handleRetryExtension}
        onOpenRepo={(url) => window.open(url, "_blank", "noopener")}
        onCheckAllUpdates={handleCheckAllUpdates}
        onInstallReference={handleInstallExtensionReference}
        catalogOffers={catalogOffers}
        catalogCoordinates={microbitCatalogCoordinateOrder}
      />
    </header>
  );
}
