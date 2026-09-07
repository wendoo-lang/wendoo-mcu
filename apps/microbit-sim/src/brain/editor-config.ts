import { assertUnreachable } from "@wendoo/core";
import { CoreTypeIds, type WendooEnvironment } from "@wendoo/core/app";
import type { BrainTileKind, IBrainTileDef } from "@wendoo/core/brain";
import type { BrainTileLiteralDef } from "@wendoo/core/brain/tiles";
import type { BrainEditorConfig, TileColorDef, TileVisual } from "@wendoo/ui";
import { adjustColor, saturateColor } from "@wendoo/ui";
import { WODAL_SHARED_TYPE_IDS } from "@wendoo/wodal";
import { imageLiteralType } from "./image-literal-type";
import { ICON_BASE, tileVisuals } from "./tile-visuals";

/** The default icon URL a tile of the given kind falls back to, or undefined when the kind has no kind-specific default. */
function defaultKindIconUrl(kind: BrainTileKind): string | undefined {
  switch (kind) {
    case "page":
      return `${ICON_BASE}/page3.svg`;
    case "undefined":
    case "sensor":
    case "actuator":
    case "parameter":
    case "operator":
    case "variable":
    case "literal":
    case "factory":
    case "controlFlow":
    case "modifier":
    case "accessor":
    case "output":
    case "missing":
      return undefined;
    default:
      return assertUnreachable(kind);
  }
}

/** How far the micro:bit chrome colors are muted for tile surfaces. */
const kTileMute = -0.1;
const kTileAdjust = -0.1;

/**
 * WHEN/DO tile color pair, the micro:bit chrome cyan and green muted by
 * {@link kTileMute}. Every tile takes this same pair, so a tile's side -- not
 * its category -- drives its color.
 */
const tileSideColors: TileColorDef = {
  when: adjustColor(saturateColor("#0adcff", kTileMute), kTileAdjust),
  do: adjustColor(saturateColor("#3affa3", kTileMute), kTileAdjust),
};

/** Data-type icon URLs keyed by type id, shared by the brain editor and the docs surfaces. */
export const microbitDataTypeIcons: ReadonlyMap<string, string> = new Map([
  [CoreTypeIds.Boolean, `${ICON_BASE}/boolean.svg`],
  [CoreTypeIds.Number, `${ICON_BASE}/number.svg`],
  [CoreTypeIds.String, `${ICON_BASE}/text.svg`],
  [WODAL_SHARED_TYPE_IDS.Image, `${ICON_BASE}/image.svg`],
]);

/**
 * The data-type icon a literal tile's value type carries in
 * {@link microbitDataTypeIcons}, and `undefined` for a tile of any other kind
 * or a value type the map does not name.
 */
function literalValueTypeIconUrl(tileDef: IBrainTileDef): string | undefined {
  if (tileDef.kind !== "literal") {
    return undefined;
  }
  return microbitDataTypeIcons.get((tileDef as BrainTileLiteralDef).valueType);
}

/**
 * Returns a tile-visual resolver that supplies the app's mapped visuals for
 * core tiles (see `tileVisuals`), falls back to the data-type icon of a
 * literal's value type and then to the tile kind's own default icon, rewrites
 * compiler-minted `/vfs/<path>` tile icon URLs to loadable URLs via
 * `resolveVfsAssetUrl`, and applies the {@link tileSideColors} WHEN/DO color
 * pair to every tile. Intrinsic visuals a tile carries in its metadata flow
 * through unchanged.
 */
export function createMicrobitTileVisualResolver(
  resolveVfsAssetUrl: (url: string) => string
): (tileDef: IBrainTileDef) => TileVisual {
  return (tileDef) => {
    const mapped = tileVisuals.get(tileDef.tileId);
    const intrinsic = tileDef.metadata as TileVisual | undefined;
    const intrinsicIconUrl = intrinsic?.iconUrl;
    const resolvedIconUrl = intrinsicIconUrl ? resolveVfsAssetUrl(intrinsicIconUrl) : undefined;
    const rewrittenIconUrl = resolvedIconUrl !== intrinsicIconUrl ? resolvedIconUrl : undefined;
    const fallbackIconUrl =
      !mapped?.iconUrl && !intrinsicIconUrl
        ? (literalValueTypeIconUrl(tileDef) ?? defaultKindIconUrl(tileDef.kind))
        : undefined;
    const visual: Partial<TileVisual> = {
      ...(intrinsic ?? {}),
      ...(mapped ?? {}),
      ...(rewrittenIconUrl !== undefined ? { iconUrl: rewrittenIconUrl } : {}),
      ...(fallbackIconUrl !== undefined ? { iconUrl: fallbackIconUrl } : {}),
      colorDef: tileSideColors,
    };
    return visual as TileVisual;
  };
}

/** Friendly data-type names keyed by type id, shared by the brain editor and the docs surfaces. */
export const microbitDataTypeNames: ReadonlyMap<string, string> = new Map([
  [CoreTypeIds.Boolean, "boolean"],
  [CoreTypeIds.Number, "number"],
  [CoreTypeIds.String, "text"],
  [WODAL_SHARED_TYPE_IDS.Image, "image"],
]);

/** What the microbit-v2 brain editor config is built from. */
export interface BuildMicrobitBrainEditorConfigOptions {
  /** The environment whose catalogs, brain services, and localizer the editor reads. */
  env: WendooEnvironment;
  /** Rewrites a compiler-minted `/vfs/<path>` tile icon URL to a loadable one. */
  resolveVfsAssetUrl: (url: string) => string;
  /** Namespace new user tiles are minted under; absent while no project is open. */
  projectNamespace?: string;
  onTileDocs?: BrainEditorConfig["onTileDocs"];
  docsIntegration?: BrainEditorConfig["docsIntegration"];
  sidePanel?: BrainEditorConfig["sidePanel"];
  libraries?: BrainEditorConfig["libraries"];
  printTransport?: BrainEditorConfig["printTransport"];
  isBrokenTile?: BrainEditorConfig["isBrokenTile"];
}

/**
 * Builds the microbit-v2 brain editor config from the app environment.
 *
 * Tile and data-type icon URLs point at `public/assets/brain/icons`. A tile
 * with no mapped or intrinsic icon resolves to the bundled `question_mark.svg`
 * missing-tile fallback. Host tiles registered by the microbit-v2 module
 * appear automatically through `tileCatalogs`. When `isBrokenTile` is supplied,
 * the editor badges each instance of a user tile whose definition failed to
 * compile.
 */
export function buildMicrobitBrainEditorConfig(options: BuildMicrobitBrainEditorConfigOptions): BrainEditorConfig {
  const { env, resolveVfsAssetUrl, projectNamespace, ...editorConfig } = options;
  return {
    ...editorConfig,
    projectNamespace,
    dataTypeIcons: microbitDataTypeIcons,
    dataTypeNames: microbitDataTypeNames,
    customLiteralTypes: [imageLiteralType],
    brainServices: env.brainServices,
    localizer: env.appServices.localizer,
    tileCatalogs: env.tileCatalogs(),
    resolveTileVisual: createMicrobitTileVisualResolver(resolveVfsAssetUrl),
  };
}
