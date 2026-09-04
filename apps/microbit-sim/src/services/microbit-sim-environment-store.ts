import {
  createIdbProjectStore,
  createJsDelivrExtensionTransport,
  createWebLocksProjectLock,
  DEFAULT_PROJECT_NAME,
  type ImportResult,
  importProjectDocument,
  type ProjectFileSystem,
  ProjectManager,
  type ProjectManifest,
  WENDOO_JSON_PATH,
} from "@wendoo/app-host";
import type { AssistantConnect, EditedBrainWorkspaces, PersonActivity } from "@wendoo/assistant-panel";
import {
  assistantSessionUrl,
  assistantToolManifest,
  createEditedBrainWorkspaces,
  createPersonActivity,
  createWebSocketConnect,
} from "@wendoo/assistant-panel";
import type { RelayToolManifest } from "@wendoo/assistant-relay";
import {
  type AppBridgeState,
  AppEnvironmentHost,
  type BrainDiagnosticEntry,
  collectBrainErrorDiagnostics,
  collectBrainTileCompileDiagnostics,
  createFolderCompileDiagnosticsPublisher,
  createVfsAssetUrlProvider,
  type ExtensionResolutionWarning,
  type FolderHostSession,
  type UserTileApplyResult,
  type VfsAssetUrlProvider,
  type WorkspaceCompileDiagnostic,
} from "@wendoo/bridge-app";
import {
  BrainDef,
  coreModule,
  createEntropySeededRng,
  createWendooEnvironment,
  logger,
  type WendooEnvironment,
} from "@wendoo/core/app";
import { createDefaultLocalizer } from "@wendoo/core/localization";
import { createProfileNumerics } from "@wendoo/core/runtime";
import { isCompilerControlledPath, type Mount } from "@wendoo/ts-compiler";
import type { PrintTransport } from "@wendoo/ui";
import {
  createWodalSharedModule,
  getWodalDeviceProfile,
  type WodalBuildInput,
  type WodalDeviceProfile,
  WodalDeviceProfileId,
} from "@wendoo/wodal";
import { createTargetAdapter } from "@wendoo/wodal/targets/microbit-v2/rehearsal";
import { name as appName } from "../../package.json";
import { type AppSettings, loadAppSettings, normalizeAppSettings, persistAppSettings } from "./app-settings";
import { loadBindingToken, saveBindingToken } from "./binding-token-persistence";
import { flashDiagnosticToEntry, runtimeFaultToEntry } from "./brain-diagnostic-entries";
import { type AppChrome, appChromeForMode, connectMicrobitFolderSession, isFolderHostMode } from "./folder-host-mode";
import { microbitDefaultExtensions, microbitEmbeddedExtensions } from "./microbit-embedded-extensions";
import { microbitFeaturedNamespaces, microbitLibraryCatalogMoves } from "./microbit-extension-browser";
import { MICROBIT_V2_TARGET_COORDINATE } from "./microbit-extension-coordinates";
import {
  BRAINS_INDEX_KEY,
  buildMicrobitSimExportDocument,
  type MicrobitSimFleet,
  SIMULATOR_STATE_KEY,
  translateMicrobitSimAppChunk,
} from "./project-io";
import { type InstanceFiberFault, MicrobitSimulator } from "./simulator";
import { SpeakerAudio } from "./speaker-audio";
import { UserCodeReflasher } from "./user-code-reflasher";

/**
 * Platform content mounts for microbit-sim, applied at the workspace root.
 * Empty until a platform mount is needed; the layer ambient `.d.ts` are
 * carried by the resolved layer extensions as their own extension content.
 */
const microbitMounts: readonly Mount[] = [];

/** Volume name a micro:bit in mass-storage mode mounts as. */
const MICROBIT_VOLUME_NAME = "MICROBIT";

/** Parses the persisted simulator fleet; returns undefined when absent or malformed. */
function parseSimulatorState(raw: string | undefined): MicrobitSimFleet | undefined {
  if (!raw) {
    return undefined;
  }
  try {
    const parsed = JSON.parse(raw) as { order?: unknown; flash?: unknown };
    if (!Array.isArray(parsed.order)) {
      return undefined;
    }
    const order = parsed.order.filter((id): id is string => typeof id === "string");
    const flash: Record<string, string> = {};
    if (parsed.flash && typeof parsed.flash === "object") {
      for (const [id, brainId] of Object.entries(parsed.flash as Record<string, unknown>)) {
        if (typeof brainId === "string") {
          flash[id] = brainId;
        }
      }
    }
    return { order, flash };
  } catch {
    return undefined;
  }
}

/** A brain's id and current display name, read from its definition, for the brain-list UI. */
export interface BrainRecord {
  readonly id: string;
  readonly name: string;
}

/** Parses the persisted brain index: an ordered JSON array of brain ids. Non-string entries are ignored. */
function parseBrainIndex(raw: string | undefined): string[] {
  if (!raw) {
    return [];
  }
  try {
    const parsed = JSON.parse(raw) as unknown;
    if (!Array.isArray(parsed)) {
      return [];
    }
    return parsed.filter((entry): entry is string => typeof entry === "string");
  } catch {
    return [];
  }
}

type AppSettingsListener = (settings: AppSettings, prev: AppSettings) => void;

// -- UiPreferences (per-project, non-portable) --

const UI_PREFS_KEY_PREFIX = `${appName}:project-ui:`;

/** Per-project UI preferences persisted in `localStorage`, keyed by project id. */
export interface UiPreferences {
  /** Whether the VS Code bridge is enabled for this project. */
  bridgeEnabled: boolean;
}

const DEFAULT_UI_PREFS: UiPreferences = {
  bridgeEnabled: false,
};

/** Loads per-project UI preferences, falling back to defaults on absence or corruption. */
function loadUiPreferences(projectId: string): UiPreferences {
  try {
    const raw = localStorage.getItem(`${UI_PREFS_KEY_PREFIX}${projectId}`);
    if (raw) {
      const parsed = JSON.parse(raw) as Partial<UiPreferences>;
      return {
        bridgeEnabled: parsed.bridgeEnabled === true,
      };
    }
  } catch {
    // corrupted data -- fall through to defaults
  }
  return { ...DEFAULT_UI_PREFS };
}

/** Persists per-project UI preferences. No-ops when storage is full or unavailable. */
function persistUiPreferences(projectId: string, prefs: UiPreferences): void {
  try {
    localStorage.setItem(`${UI_PREFS_KEY_PREFIX}${projectId}`, JSON.stringify(prefs));
  } catch {
    // storage full or unavailable
  }
}

// -- Assistant composition --

/** What this app stands an assistant over: what it declares, what a turn edits, and how a session opens. */
export interface AssistantComposition {
  /** What the handshake declares this app serves. */
  readonly manifest: RelayToolManifest;
  /** The workspaces a turn's tool calls run against, following the editor's working copy. */
  readonly workspaces: EditedBrainWorkspaces;
  /** Where the person's own acting on the edited brain is recorded, read by the panel and the workspaces alike. */
  readonly activity: PersonActivity;
  /** Opens one relay session against the service address the settings hold at the time of the call. */
  readonly connect: AssistantConnect;
}

/**
 * Owns the long-lived microbit-sim app state: the caller-owned Wendoo
 * environment, the durable project lifecycle, and the user-managed brain list.
 *
 * Brains are a flat, dynamic list keyed by UUID with editable display names.
 * There is no archetype concept. React components consume the store through the
 * environment context. The store wraps {@link AppEnvironmentHost} and is
 * constructed through {@link create}.
 */
export class MicrobitSimEnvironmentStore {
  readonly host: AppEnvironmentHost;

  /** The simulated microbit fleet (instances, shared medium, tick driver). */
  readonly simulator: MicrobitSimulator;

  /** Renders each device's built-in sound emoji audibly through the Web Audio API. */
  private readonly _speakerAudio = new SpeakerAudio();

  /** The device profile this app instance runs: drives the environment module, builds, import, and export. */
  readonly activeDeviceProfile: WodalDeviceProfile;

  /** Reflashes flashed instances when a user-code recompile changes an action their brain uses. */
  private readonly _userCodeReflasher: UserCodeReflasher;

  private _brainIds: readonly string[] = [];
  private _brains: readonly BrainRecord[] = [];
  private _restoringFleet = false;
  private readonly _brainsListeners = new Set<() => void>();
  private readonly _activeProjectListeners = new Set<() => void>();

  /** Bounded per-brain buffer of the latest VM fiber faults, newest last. Keyed by brain id. */
  private readonly _runtimeFaults = new Map<string, readonly BrainDiagnosticEntry[]>();
  private _runtimeFaultsRevision = 0;
  private readonly _runtimeFaultsListeners = new Set<() => void>();

  /** Maximum faults retained per brain in {@link _runtimeFaults}; older faults are dropped. */
  private static readonly RUNTIME_FAULT_CAP = 20;

  private _appSettings: AppSettings = loadAppSettings();
  private readonly _appSettingsListeners = new Set<AppSettingsListener>();

  /**
   * Whether the brain editor's assistant panel stands open, keyed by brain
   * document id. Held for as long as this store runs and stored nowhere, so a
   * reload starts every brain closed.
   */
  private readonly _assistantPanelOpenByBrain = new Map<string, boolean>();

  private _uiPreferences: UiPreferences = { ...DEFAULT_UI_PREFS };

  private _vfsRevisionWiringInitialized = false;
  private readonly _vfsAssetUrlProvider: VfsAssetUrlProvider;
  private _isSwitchingProject = false;
  private readonly _folderSession: FolderHostSession | undefined;
  private readonly _chrome: AppChrome;
  private readonly _printTransport: PrintTransport | undefined;

  /** What an assistant provider over this app is built from, standing for the life of the store. */
  readonly assistant: AssistantComposition;

  private constructor(
    host: AppEnvironmentHost,
    activeDeviceProfile: WodalDeviceProfile,
    folderSession: FolderHostSession | undefined
  ) {
    this.host = host;
    this.activeDeviceProfile = activeDeviceProfile;
    this._folderSession = folderSession;
    this._chrome = appChromeForMode(folderSession !== undefined);
    // In folder host mode the app runs in a webview iframe whose sandbox blocks
    // window.print(); route printing to the host, which opens the document in
    // the system browser. In browser mode printing stays the default window.print().
    this._printTransport = folderSession
      ? (html) => {
          void folderSession.openExternalDocument(html);
        }
      : undefined;
    this._vfsAssetUrlProvider = createVfsAssetUrlProvider({
      getProjectFileSystem: () => this.host.servedProjectFileSystem,
      getVfsRevision: () => this.host.getVfsRevisionSnapshot(),
    });
    const adapter = createTargetAdapter(MICROBIT_V2_TARGET_COORDINATE);
    const activity = createPersonActivity();
    this.assistant = {
      manifest: assistantToolManifest(adapter),
      activity,
      workspaces: createEditedBrainWorkspaces({
        environment: host.env,
        adapter,
        activity,
        featuring: () => ({
          featured: microbitFeaturedNamespaces,
          ...(host.activeProjectManifest ? { hostNamespace: host.activeProjectManifest.id } : {}),
        }),
      }),
      connect: () => createWebSocketConnect(assistantSessionUrl(this._appSettings.assistantServiceUrl))(),
    };
    this.simulator = new MicrobitSimulator(host.env);
    this._userCodeReflasher = new UserCodeReflasher({
      flashedBrainIds: () =>
        this.simulator
          .getInstances()
          .map((instance) => instance.flashedBrainId)
          .filter((id): id is string => id !== undefined),
      getBrainDef: (brainId) => this.host.getCachedBrain(brainId),
      reflashBrain: (brainId) => {
        void this.reflashBrain(brainId);
      },
    });
    // The instance-list listener fires on fleet changes (add/remove/flash), not on per-tick device updates.
    this.simulator.subscribeToInstances(() => {
      void this.persistSimulatorState();
      this._speakerAudio.retain(new Set(this.simulator.getInstances().map((instance) => instance.id)));
    });
    // Route each instance's VM fiber faults into the faulting brain's runtime-fault buffer.
    this.simulator.subscribeToFiberFaults((fault) => {
      this.recordRuntimeFault(fault);
    });
    // Feed each device's speaker snapshot to the audio renderer every frame.
    this.simulator.subscribeToFrame(() => {
      for (const instance of this.simulator.getInstances()) {
        this._speakerAudio.sync(instance.id, instance.snapshot().speaker.playing);
      }
    });
    this.host.onProjectLoaded(() => {
      const prefs = loadUiPreferences(this.host.projectManager.activeProject!.manifest.id);
      // Switching to a different project must not silently auto-connect its bridge; a fresh load
      // keeps the saved toggle so a reloaded session reconnects.
      this._uiPreferences = this._isSwitchingProject ? { ...prefs, bridgeEnabled: false } : prefs;
      void this.reloadProjectState();
    });
    if (folderSession) {
      this.host.onCompilerControlledFilesChanged(() => {
        this.publishCompilerControlledFiles();
      });
    }
  }

  /** Creates the store with the core module and the active device profile's module installed. */
  static async create(): Promise<MicrobitSimEnvironmentStore> {
    // The one place this app declares its device; the module and the build profile both derive from it.
    const activeProfile = getWodalDeviceProfile(WodalDeviceProfileId.MICROBIT_V2);
    const appSettings = loadAppSettings();
    const folderSession = isFolderHostMode() ? await connectMicrobitFolderSession() : undefined;
    const publishFolderDiagnostics = folderSession ? createFolderCompileDiagnosticsPublisher(folderSession) : undefined;
    const projectStore = folderSession ? folderSession.store : await createIdbProjectStore(appName);
    let instanceRef: MicrobitSimEnvironmentStore | undefined;
    const host = new AppEnvironmentHost({
      projectManager: new ProjectManager(projectStore, {
        filesystemOptions: {
          shouldExclude: (path) => isCompilerControlledPath(path, microbitMounts),
        },
        ...(folderSession ? {} : { lock: createWebLocksProjectLock(appName) }),
        defaultExtensions: microbitDefaultExtensions,
      }),
      modules: [coreModule(), createWodalSharedModule(), activeProfile.createWendooModule()],
      numerics: createProfileNumerics(activeProfile.numberPrecision),
      localizer: createDefaultLocalizer(),
      mounts: microbitMounts,
      embeddedExtensions: microbitEmbeddedExtensions,
      extensionFetchTransport: createJsDelivrExtensionTransport(),
      catalogMoves: microbitLibraryCatalogMoves,
      ...(folderSession ? {} : { bridgeUrl: appSettings.vscodeBridgeUrl }),
      loadBindingToken,
      saveBindingToken,
      rng: createEntropySeededRng(),
      onDidCompile: (result, tileResult) => {
        instanceRef?.handleProjectCompiled(tileResult);
        publishFolderDiagnostics?.(result.files);
      },
    });
    const instance = new MicrobitSimEnvironmentStore(host, activeProfile, folderSession);
    instanceRef = instance;
    instance._appSettings = appSettings;
    return instance;
  }

  /** Opens or creates the durable default project. */
  async initialize(): Promise<void> {
    await this.host.initialize(DEFAULT_PROJECT_NAME);
    const activeProject = this.host.projectManager.activeProject;
    if (activeProject) {
      this._uiPreferences = loadUiPreferences(activeProject.manifest.id);
    }
    await this.reloadProjectState();
    if (!this._vfsRevisionWiringInitialized) {
      this.initVfsRevisionWiring();
      this._vfsRevisionWiringInitialized = true;
    }
    if (this._folderSession) {
      this._folderSession.onExternalChange((change) => {
        this.host.applyExternalProjectFileChange(change);
        if (change.action === "write" && change.path === WENDOO_JSON_PATH) {
          void this.refreshFromExternalManifest();
        }
      });
      this.publishCompilerControlledFiles();
      return;
    }
    this.host.initBridge();
    this.onAppSettingsChange((settings, prev) => {
      if (settings.vscodeBridgeUrl !== prev.vscodeBridgeUrl) {
        this.host.updateBridgeUrl(settings.vscodeBridgeUrl);
      }
    });
  }

  /** Visibility of the app's top-level chrome sections for the current mode. */
  get chrome(): AppChrome {
    return this._chrome;
  }

  /**
   * Print sink for the current mode: a transport routing the printable
   * document to the host in folder mode, or undefined in browser mode (where
   * printing uses `window.print()`).
   */
  get printTransport(): PrintTransport | undefined {
    return this._printTransport;
  }

  /**
   * Bumps the VFS revision on every local file-system change, re-subscribing
   * to the new project's file system on each project load.
   */
  private initVfsRevisionWiring(): void {
    let unsubLocalChange = this.projectFileSystem.onLocalChange(() => this.bumpVfsRevision());
    this.host.onProjectLoaded(() => {
      unsubLocalChange();
      unsubLocalChange = this.projectFileSystem.onLocalChange(() => this.bumpVfsRevision());
      this.bumpVfsRevision();
    });
  }

  /** Caller-owned Wendoo environment shared across the app. */
  get env(): WendooEnvironment {
    return this.host.env;
  }

  /** Durable project lifecycle manager. */
  get projectManager(): ProjectManager {
    return this.host.projectManager;
  }

  /** Active project file system. */
  get projectFileSystem(): ProjectFileSystem {
    return this.host.projectFileSystem;
  }

  /**
   * File system the VFS asset-url provider resolves assets from: the raw
   * project files plus compiler-controlled files, including the
   * installed-extensions tree, so extension tile icons and docs resolve.
   */
  get servedProjectFileSystem(): ProjectFileSystem {
    return this.host.servedProjectFileSystem;
  }

  /** Manifest of the active project, or undefined when none is open. */
  get activeProjectManifest(): ProjectManifest | undefined {
    return this.host.activeProjectManifest;
  }

  /** Registers a listener fired after a project finishes loading. */
  onProjectLoaded(listener: () => void): () => void {
    return this.host.onProjectLoaded(listener);
  }

  /** Registers a listener fired before the active project unloads. */
  onProjectUnloading(listener: () => void): () => void {
    return this.host.onProjectUnloading(listener);
  }

  /** Creates a durable project and makes it active. */
  async createProject(name: string): Promise<void> {
    this._isSwitchingProject = true;
    try {
      await this.host.createProject(name);
    } finally {
      this._isSwitchingProject = false;
    }
  }

  /** Switches the active project within the current collection. */
  async switchProject(id: string): Promise<void> {
    this._isSwitchingProject = true;
    try {
      await this.host.switchProject(id);
    } finally {
      this._isSwitchingProject = false;
    }
  }

  /** Renames the active project. */
  async renameProject(name: string): Promise<void> {
    await this.host.updateProjectMetadata({ name });
    for (const listener of this._activeProjectListeners) {
      listener();
    }
  }

  /** Lists durable projects in the active collection. */
  async listProjects(): Promise<ProjectManifest[]> {
    return this.host.projectManager.listProjects();
  }

  /** Imports a `.wendoo` file as a new durable project and switches to it on success. */
  async importProject(file: File): Promise<ImportResult> {
    const result = await importProjectDocument(file, appName, this.host.projectManager, {
      appChunkCallback: translateMicrobitSimAppChunk,
    });
    if (result.success && result.projectId) {
      await this.switchProject(result.projectId);
    }
    return result;
  }

  /** Exports the active project as a shared `.wendoo` document string. */
  async exportProject(): Promise<string> {
    return buildMicrobitSimExportDocument(this.host.projectManager, {
      brainOrder: [...this._brainIds],
      simulator: this.simulatorStateSnapshot(),
    });
  }

  /** Subscribes to active-project changes for `useSyncExternalStore`. */
  subscribeToActiveProject = (listener: () => void): (() => void) => {
    this._activeProjectListeners.add(listener);
    const unsubscribe = this.host.projectManager.onActiveProjectChange(listener);
    return () => {
      this._activeProjectListeners.delete(listener);
      unsubscribe();
    };
  };

  /** Snapshot of the active project name for `useSyncExternalStore`. */
  getActiveProjectName = (): string => {
    return this.host.activeProjectManifest?.name ?? "(no project)";
  };

  /** Creates a brain seeded from an empty definition. */
  async addBrain(name: string): Promise<string> {
    const brainDef = this.env.withServices((services) => BrainDef.emptyBrainDef(services, name));
    // Invariant: microbit-sim keys host brain storage by the brain's own id.
    const id = brainDef.id();
    await this.host.saveBrainForKey(id, brainDef);
    this._brainIds = [...this._brainIds, id];
    await this.persistBrainIndex();
    this.rebuildBrains();
    this.notifyBrainsChanged();
    return id;
  }

  /**
   * Imports a brain from a serialized `.brain` file and adds it to the brain list.
   * A file id already used by an existing brain is replaced with a fresh one.
   * Returns the imported brain's id. Throws when the file is not a valid brain.
   */
  async importBrain(file: File): Promise<string> {
    const plain = JSON.parse(await file.text()) as { id?: unknown };
    if (typeof plain.id === "string" && this._brainIds.includes(plain.id)) {
      plain.id = undefined;
    }
    const brainDef = this.env.deserializeBrainJsonFromPlain(plain, this.activeProjectId());
    if (brainDef.pages().size() === 0) {
      brainDef.appendNewPage();
    }
    const id = brainDef.id();
    await this.host.saveBrainForKey(id, brainDef);
    this._brainIds = [...this._brainIds, id];
    await this.persistBrainIndex();
    this.rebuildBrains();
    this.notifyBrainsChanged();
    return id;
  }

  /** Removes a brain and its stored definition, unflashing any instance running it and dropping its runtime faults. */
  async removeBrain(id: string): Promise<void> {
    await this.host.removeBrain(id);
    this._brainIds = this._brainIds.filter((brainId) => brainId !== id);
    await this.persistBrainIndex();
    this.rebuildBrains();
    this.notifyBrainsChanged();
    this.simulator.unflash(id);
    if (this._runtimeFaults.delete(id)) {
      this._runtimeFaultsRevision++;
      for (const listener of this._runtimeFaultsListeners) {
        listener();
      }
    }
  }

  /** Renames a brain by updating its definition. */
  async renameBrain(id: string, name: string): Promise<void> {
    const brainDef = this.host.getCachedBrain(id) ?? (await this.getBrain(id));
    if (!brainDef) {
      return;
    }
    brainDef.setName(name);
    await this.host.saveBrainForKey(id, brainDef);
    this.rebuildBrains();
    this.notifyBrainsChanged();
  }

  /** Loads a brain definition by id. */
  async getBrain(id: string): Promise<BrainDef | undefined> {
    return (await this.host.loadBrainFromProject(id)) as BrainDef | undefined;
  }

  /** Namespace of the active project; persisted brain JSON is relative to it. */
  private activeProjectId(): string {
    return this.host.projectManager.activeProject!.manifest.id;
  }

  /** Serializes a brain into portable `.brain` JSON text, or undefined when it cannot be loaded. */
  async exportBrainSource(id: string): Promise<string | undefined> {
    const brainDef = this.host.getCachedBrain(id) ?? (await this.getBrain(id));
    if (!brainDef) {
      return undefined;
    }
    return JSON.stringify(this.host.serializeBrainForStorage(brainDef), null, 2);
  }

  /** Persists an edited brain definition (replacing the cached instance) and re-flashes instances running it. */
  async saveBrain(id: string, brainDef: BrainDef): Promise<void> {
    await this.host.saveBrainForKey(id, brainDef);
    this.rebuildBrains();
    this.notifyBrainsChanged();
    await this.reflashBrain(id);
  }

  /**
   * Returns the live build input for a specific brain by id, or undefined when it
   * cannot be loaded.
   */
  async getBuildInputForBrain(brainId: string): Promise<WodalBuildInput | undefined> {
    return this.buildInputForBrain(brainId);
  }

  /** Assembles the WODAL build input for a specific brain, or undefined when it cannot be loaded. */
  private async buildInputForBrain(brainId: string): Promise<WodalBuildInput | undefined> {
    const brainDef = this.host.getCachedBrain(brainId);
    if (!brainDef) {
      return undefined;
    }
    return {
      brainDef,
      environment: this.env,
      deviceProfile: this.activeDeviceProfile,
    };
  }

  /** Builds the given brain and flashes it onto the given simulator instance. A no-op when the brain cannot be loaded. */
  async flashBrainToInstance(instanceId: string, brainId: string): Promise<void> {
    const input = await this.buildInputForBrain(brainId);
    if (!input) {
      return;
    }
    this.simulator.flash(instanceId, input, brainId);
  }

  /** Restarts the instance's associated brain from a fresh build. A no-op when no brain is associated. */
  async resetInstance(instanceId: string): Promise<void> {
    const instance = this.simulator.getInstances().find((candidate) => candidate.id === instanceId);
    const brainId = instance?.flashedBrainId;
    if (!brainId) {
      return;
    }
    const input = await this.buildInputForBrain(brainId);
    if (!input) {
      return;
    }
    this.simulator.flash(instanceId, input, brainId);
  }

  /** Re-flashes every instance associated with `brainId` -- loaded or failed -- from the brain's latest definition. */
  async reflashBrain(brainId: string): Promise<void> {
    const hasTargets = this.simulator.getInstances().some((instance) => instance.flashedBrainId === brainId);
    if (!hasTargets) {
      return;
    }
    const input = await this.buildInputForBrain(brainId);
    if (!input) {
      return;
    }
    this.simulator.reflash(brainId, input);
  }

  /** True when brains flash to a physical micro:bit by writing the built hex onto its USB drive. */
  get flashesViaMicrobitDrive(): boolean {
    return this._folderSession !== undefined;
  }

  /**
   * Writes a built firmware hex file to the root of the mounted MICROBIT
   * drive. Rejects with a `FolderSessionError` carrying
   * `REMOVABLE_VOLUME_NOT_FOUND` when the drive is not mounted, or
   * `WRITE_FAILED` when the copy fails.
   */
  async writeHexToMicrobitDrive(filename: string, hex: string): Promise<void> {
    if (!this._folderSession) {
      throw new Error("Flashing to the micro:bit drive requires a folder session");
    }
    await this._folderSession.writeRemovableVolumeFile(MICROBIT_VOLUME_NAME, filename, hex);
  }

  /**
   * Reacts to a finished project compile by reflashing flashed instances whose brains use a user
   * action the compile changed.
   */
  private handleProjectCompiled(tileResult: UserTileApplyResult | undefined): void {
    this._userCodeReflasher.onActionsChanged(tileResult?.changedActionKeys ?? []);
  }

  /** Subscribes to brain-list or selection changes for `useSyncExternalStore`. */
  subscribeToBrains = (listener: () => void): (() => void) => {
    this._brainsListeners.add(listener);
    return () => {
      this._brainsListeners.delete(listener);
    };
  };

  /** Snapshot of the brain list for `useSyncExternalStore`. */
  getBrains = (): readonly BrainRecord[] => {
    return this._brains;
  };

  /** Subscribes to brain-diagnostics revision changes for `useSyncExternalStore`. Returns an unsubscribe function. */
  subscribeToBrainDiagnostics = (listener: () => void): (() => void) => {
    return this.host.subscribeToBrainDiagnostics(listener);
  };

  /** Snapshot of the current brain-diagnostics revision for `useSyncExternalStore`. */
  getBrainDiagnosticsRevision = (): number => {
    return this.host.getBrainDiagnosticsRevision();
  };

  /** Subscribes to workspace-compile diagnostic changes for `useSyncExternalStore`. Returns an unsubscribe function. */
  subscribeToCompileDiagnostics = (listener: () => void): (() => void) => {
    return this.host.subscribeToCompileDiagnostics(listener);
  };

  /** Snapshot of the latest workspace compile's diagnostics for `useSyncExternalStore`; empty when clean. */
  getCompileDiagnosticsSnapshot = (): readonly WorkspaceCompileDiagnostic[] => {
    return this.host.getCompileDiagnosticsSnapshot();
  };

  /**
   * The verbatim error diagnostics a brain surfaces: the stored per-rule
   * typecheck errors, the compile diagnostics of any broken user tile the brain
   * uses (deduplicated per distinct tile key), and the latest build/link/load
   * failure of any instance flashed from the brain. These are the persistent
   * compile/flash problems that drive the brain-list badge. Empty when the brain
   * is not cached or is clean. Transient runtime faults are not included here;
   * see {@link getBrainRuntimeFaults}.
   */
  getBrainDiagnostics(brainId: string): readonly BrainDiagnosticEntry[] {
    const brain = this.host.getCachedBrain(brainId);
    if (!brain) {
      return [];
    }
    return [
      ...collectBrainErrorDiagnostics(brain),
      ...collectBrainTileCompileDiagnostics(brain, (key) => this.host.getTileCompileDiagnostics(key)),
      ...this.simulator.flashDiagnosticsForBrain(brainId).map(flashDiagnosticToEntry),
    ];
  }

  /**
   * Whether the brain currently surfaces any error diagnostic, i.e. whether
   * {@link getBrainDiagnostics} is non-empty. A status boolean for surfaces that
   * reflect a brain's error state without exposing the error details. Transient
   * runtime faults are not counted; see {@link getBrainRuntimeFaults}.
   */
  brainHasErrors(brainId: string): boolean {
    return this.getBrainDiagnostics(brainId).length > 0;
  }

  /**
   * The bounded buffer of recent VM fiber faults raised by instances flashed
   * from this brain, newest last, verbatim. These are transient runtime events
   * shown only in the expanded brain panel; they do not affect the compile
   * badge. Empty when the brain has no recorded faults.
   */
  getBrainRuntimeFaults(brainId: string): readonly BrainDiagnosticEntry[] {
    return this._runtimeFaults.get(brainId) ?? [];
  }

  /** Subscribes to runtime-fault buffer changes for `useSyncExternalStore`. Returns an unsubscribe function. */
  subscribeToRuntimeFaults = (listener: () => void): (() => void) => {
    this._runtimeFaultsListeners.add(listener);
    return () => {
      this._runtimeFaultsListeners.delete(listener);
    };
  };

  /** Snapshot of the current runtime-fault revision for `useSyncExternalStore`; bumped on each recorded fault. */
  getRuntimeFaultsRevision = (): number => {
    return this._runtimeFaultsRevision;
  };

  /** Appends a fiber fault to its brain's bounded buffer and notifies subscribers. Faults with no brain are dropped. */
  private recordRuntimeFault(fault: InstanceFiberFault): void {
    if (!fault.brainId) {
      return;
    }
    const existing = this._runtimeFaults.get(fault.brainId) ?? [];
    const next = [...existing, runtimeFaultToEntry(fault)];
    const capped =
      next.length > MicrobitSimEnvironmentStore.RUNTIME_FAULT_CAP
        ? next.slice(next.length - MicrobitSimEnvironmentStore.RUNTIME_FAULT_CAP)
        : next;
    this._runtimeFaults.set(fault.brainId, capped);
    this._runtimeFaultsRevision++;
    for (const listener of this._runtimeFaultsListeners) {
      listener();
    }
  }

  // -- App settings (global) --

  /** Snapshot of the current global app settings for `useSyncExternalStore`. */
  getAppSettings = (): AppSettings => {
    return this._appSettings;
  };

  /** Subscribes to app-settings changes for `useSyncExternalStore`. Returns an unsubscribe function. */
  subscribeToAppSettings = (listener: () => void): (() => void) => {
    return this.onAppSettingsChange(listener);
  };

  /**
   * Merges a patch into the global app settings, persists the result, and notifies listeners.
   * A blank address field is reset to its default.
   */
  updateAppSettings(patch: Partial<AppSettings>): void {
    const prev = this._appSettings;
    this._appSettings = normalizeAppSettings({ ...this._appSettings, ...patch });
    persistAppSettings(this._appSettings);
    for (const fn of this._appSettingsListeners) {
      fn(this._appSettings, prev);
    }
  }

  /** Registers a listener fired after app settings change. Returns an unsubscribe function. */
  onAppSettingsChange(fn: AppSettingsListener): () => void {
    this._appSettingsListeners.add(fn);
    return () => {
      this._appSettingsListeners.delete(fn);
    };
  }

  // -- Assistant panel (per brain, for as long as the app runs) --

  /** Whether the assistant panel stood open the last time `brainId` was edited; false for a brain it was never opened for. */
  isAssistantPanelOpen(brainId: string): boolean {
    return this._assistantPanelOpenByBrain.get(brainId) === true;
  }

  /** Notes that the assistant panel stands `open` while `brainId` is edited. */
  setAssistantPanelOpen(brainId: string, open: boolean): void {
    this._assistantPanelOpenByBrain.set(brainId, open);
  }

  // -- UI preferences (per-project) --

  /** Returns the active project's UI preferences. */
  getUiPreferences(): UiPreferences {
    return this._uiPreferences;
  }

  /** Merges a patch into the active project's UI preferences and persists the result. */
  updateUiPreferences(patch: Partial<UiPreferences>): void {
    this._uiPreferences = { ...this._uiPreferences, ...patch };
    const projectId = this.host.projectManager.activeProject?.manifest.id;
    if (projectId) {
      persistUiPreferences(projectId, this._uiPreferences);
    }
  }

  // -- Doc revision (delegate) --

  /** Subscribes to doc-revision changes (bumped when user tiles install) for `useSyncExternalStore`. */
  subscribeToDocRevision = (listener: () => void): (() => void) => {
    return this.host.subscribeToDocRevision(listener);
  };

  /** Snapshot of the current doc revision for `useSyncExternalStore`. */
  getDocRevisionSnapshot = (): number => {
    return this.host.getDocRevisionSnapshot();
  };

  // -- Extension resolution warnings (delegate) --

  /** Subscribes to extension-resolution warning changes for `useSyncExternalStore`. Returns an unsubscribe function. */
  subscribeToResolutionWarnings = (listener: () => void): (() => void) => {
    return this.host.subscribeToResolutionWarnings(listener);
  };

  /** Snapshot of the active project's latest extension-resolution warnings for `useSyncExternalStore`. */
  getResolutionWarningsSnapshot = (): readonly ExtensionResolutionWarning[] => {
    return this.host.getResolutionWarningsSnapshot();
  };

  // -- VFS revision (delegate) --

  /** Bumps the VFS revision, signaling subscribers that the project filesystem snapshot changed. */
  bumpVfsRevision(): void {
    this.host.bumpVfsRevision();
  }

  /** Subscribes to VFS revision changes for `useSyncExternalStore`. Returns an unsubscribe function. */
  subscribeToVfsRevision = (listener: () => void): (() => void) => {
    return this.host.subscribeToVfsRevision(listener);
  };

  /** Snapshot of the current VFS revision for `useSyncExternalStore`. */
  getVfsRevisionSnapshot = (): number => {
    return this.host.getVfsRevisionSnapshot();
  };

  /**
   * Resolves a compiler-minted `/vfs/<path>` asset URL to an object URL over
   * the served project file system, cached per VFS revision. Other URLs pass
   * through unchanged.
   */
  resolveVfsAssetUrl(url: string): string {
    return this._vfsAssetUrlProvider.resolveAssetUrl(url);
  }

  // -- Bridge (delegate) --

  /** Starts the VS Code bridge connection, lazily initializing the bridge handle if needed. */
  connectBridge(): void {
    this.host.connectBridge();
  }

  /** Stops the VS Code bridge connection. */
  disconnectBridge(): void {
    this.host.disconnectBridge();
  }

  /** Subscribes to bridge connection-status changes for `useSyncExternalStore`. */
  subscribeToBridgeStatus = (listener: () => void): (() => void) => {
    return this.host.subscribeToBridgeStatus(listener);
  };

  /** Snapshot of the current bridge connection status for `useSyncExternalStore`. */
  getBridgeStatusSnapshot = (): AppBridgeState => {
    return this.host.getBridgeStatusSnapshot();
  };

  /** Subscribes to bridge join-code changes for `useSyncExternalStore`. */
  subscribeToBridgeJoinCode = (listener: () => void): (() => void) => {
    return this.host.subscribeToBridgeJoinCode(listener);
  };

  /** Snapshot of the current bridge join code, or undefined when none is active. */
  getBridgeJoinCodeSnapshot = (): string | undefined => {
    return this.host.getBridgeJoinCodeSnapshot();
  };

  /**
   * Unlocks simulator audio on a user gesture. Browsers block the Web Audio API
   * until the user interacts with the page; call this from the first click or
   * keypress so built-in sounds become audible.
   */
  unlockAudio(): void {
    this._speakerAudio.unlock();
  }

  /** Releases host resources owned by this store. */
  dispose(): void {
    this._userCodeReflasher.cancelPending();
    this._speakerAudio.dispose();
    this._folderSession?.dispose();
    this.host.dispose();
  }

  /** Restores brain list and simulator fleet from the active project. Runs after the host loads its brain cache. */
  private async reloadProjectState(): Promise<void> {
    await this.reloadBrains();
    await this.reloadSimulatorState();
  }

  /**
   * Publishes the compiler-controlled file set and the installed
   * fetched-extension provenance to the folder-session host. A no-op outside
   * a folder session or before the compiler is wired.
   */
  private publishCompilerControlledFiles(): void {
    if (!this._folderSession) {
      return;
    }
    const files = this.host.getCompilerControlledFiles();
    if (!files) {
      return;
    }
    this._folderSession.publishCompilerControlledFiles(files, this.host.getInstalledExtensionMetadata());
  }

  /**
   * Applies an external `wendoo.json` edit to the live app state: the
   * brain cache reconciles against the stored brains chunk (changed brains
   * re-flash, removed brains unflash), the brain list reloads, and a changed
   * simulator chunk restores the fleet.
   */
  private async refreshFromExternalManifest(): Promise<void> {
    const { changed, removed } = await this.host.reconcileBrainsFromStore();
    await this.reloadBrains();
    for (const id of removed) {
      this.simulator.unflash(id);
    }
    for (const id of changed) {
      await this.reflashBrain(id);
    }
    const raw = await this.readAppData(SIMULATOR_STATE_KEY);
    const stored = parseSimulatorState(raw);
    if (stored && JSON.stringify(stored) !== JSON.stringify(this.simulatorStateSnapshot())) {
      await this.reloadSimulatorState();
    }
  }

  /**
   * Read one of the active project's app-data records. Returns undefined when
   * the record is absent and when the store could not serve it, so a store
   * that cannot be read restores the same state an empty store does. The
   * failure the host recorded for the load is what withholds the writes.
   *
   * @param key - App-data key to read.
   */
  private async readAppData(key: string): Promise<string | undefined> {
    try {
      return await this.host.projectManager.loadAppData(key);
    } catch (error) {
      logger.warn(`Failed to read project app data "${key}":`, error);
      return undefined;
    }
  }

  private async reloadBrains(): Promise<void> {
    const raw = await this.readAppData(BRAINS_INDEX_KEY);
    const indexed = parseBrainIndex(raw);
    // Reconcile against the host cache (the source of truth for which brains exist): keep the index
    // order for brains that exist, drop dangling ids, and append cached brains the index omits (e.g. an
    // imported or payload-less document), so no brain is unreachable.
    const cached = this.host.getCachedBrainKeys();
    const cachedSet = new Set(cached);
    const ordered = indexed.filter((id) => cachedSet.has(id));
    const orderedSet = new Set(ordered);
    this._brainIds = [...ordered, ...cached.filter((id) => !orderedSet.has(id))];
    const changed = this._brainIds.length !== indexed.length || this._brainIds.some((id, i) => id !== indexed[i]);
    // An unreadable brain record leaves the cache empty, so the reconciled order
    // omits every stored brain; persisting it would drop the saved order.
    if (changed && !this.host.projectRecordFailure) {
      await this.persistBrainIndex();
    }
    this.rebuildBrains();
    this.notifyBrainsChanged();
  }

  /** Rebuilds the derived brain-list snapshot from the host's brain cache. Names are read live from the defs. */
  private rebuildBrains(): void {
    this._brains = this._brainIds.map((id) => ({ id, name: this.host.getCachedBrain(id)?.name() ?? "" }));
  }

  private async persistBrainIndex(): Promise<void> {
    await this.host.projectManager.saveAppData(BRAINS_INDEX_KEY, JSON.stringify(this._brainIds));
  }

  /**
   * Restores the simulator fleet from durable state: recreates the saved instances in order and
   * re-flashes each from its brain. An instance whose brain no longer exists is left empty. Absent or
   * malformed state restores a single empty instance.
   */
  private async reloadSimulatorState(): Promise<void> {
    const raw = await this.readAppData(SIMULATOR_STATE_KEY);
    const state = parseSimulatorState(raw);
    this._restoringFleet = true;
    try {
      if (!state || state.order.length === 0) {
        // No saved fleet: one fresh empty instance (the simulator mints its id).
        this.simulator.setInstances([]);
        this.simulator.addInstance();
      } else {
        const instances = this.simulator.setInstances(state.order);
        for (const instance of instances) {
          const brainId = state.flash[instance.id];
          if (!brainId) {
            continue;
          }
          const input = await this.buildInputForBrain(brainId);
          if (input) {
            this.simulator.flash(instance.id, input, brainId);
          }
        }
      }
    } finally {
      this._restoringFleet = false;
    }
    if (!state || state.order.length === 0) {
      // Persist the freshly minted default fleet so its id is reused on the next load.
      await this.persistSimulatorState();
    }
    // Drop any reflash the load-time compile queued.
    this._userCodeReflasher.cancelPending();
  }

  /** The current fleet as a persistable snapshot: ordered instance ids and the brain each is associated with. */
  private simulatorStateSnapshot(): MicrobitSimFleet {
    const instances = this.simulator.getInstances();
    const order = instances.map((instance) => instance.id);
    const flash: Record<string, string> = {};
    for (const instance of instances) {
      const brainId = instance.flashedBrainId;
      if (brainId) {
        flash[instance.id] = brainId;
      }
    }
    return { order, flash };
  }

  /**
   * Persists the current fleet as durable project state. Persists nothing
   * while a restore is in flight, and nothing while the load hit a stored
   * record the store could not serve: the fleet then holds one fresh instance
   * and would replace the saved one.
   */
  private async persistSimulatorState(): Promise<void> {
    if (this._restoringFleet || this.host.projectRecordFailure) {
      return;
    }
    await this.host.projectManager.saveAppData(SIMULATOR_STATE_KEY, JSON.stringify(this.simulatorStateSnapshot()));
  }

  private notifyBrainsChanged(): void {
    for (const listener of this._brainsListeners) {
      listener();
    }
  }
}
