import {
  BrainTileLiteralDef,
  CoreTypeIds,
  List,
  mkLiteralFactoryTileId,
  type StructValue,
  type WendooModule,
  type WendooModuleApi,
} from "@wendoo/core/app";
import { mintDocumentId } from "@wendoo/core/brain/model";
import { BrainTileFactoryDef } from "@wendoo/core/brain/tiles";
import { imageValueFromDigits, isImageStructValue } from "./image-value";
import { ImageField, WODAL_SHARED_TYPE_IDS, WodalSharedTypeAtomId } from "./shared-type-ids";

/** Wendoo module ID for the wodal-shared types installed by every target. */
export const WODAL_SHARED_MODULE_ID = "wendoo.wodal-shared";

/**
 * Factory identifier of the `Image` literal factory tile the wodal-shared
 * module registers. Its tile id is `mkLiteralFactoryTileId` of this value.
 */
export const WODAL_IMAGE_LITERAL_FACTORY_ID = "image";

/**
 * Creates the wodal-shared Wendoo module: nominal types common to every
 * wodal/codal target, plus the literal factories that mint their values.
 * Install it into an environment together with the core module and one target
 * module.
 */
export function createWodalSharedModule(): WendooModule {
  return {
    id: WODAL_SHARED_MODULE_ID,
    install(api: WendooModuleApi): void {
      registerSharedTypes(api);
      registerImageLiteralFactory(api);
    },
  };
}

function registerSharedTypes(api: WendooModuleApi): void {
  const { types } = api.brainServices.runtime;

  types.addStructType("Image", {
    atomId: WodalSharedTypeAtomId.Image,
    fields: List.from([
      { name: "width", typeId: CoreTypeIds.Number, fieldIndex: ImageField.Width },
      { name: "height", typeId: CoreTypeIds.Number, fieldIndex: ImageField.Height },
      { name: "pixels", typeId: CoreTypeIds.Buffer, fieldIndex: ImageField.Pixels },
    ]),
  });
}

/**
 * The `Image` struct value a manufacture option names: an `Image` struct value
 * as it stands, or the image a pixel-digit string spells. `undefined` for
 * anything else.
 */
function mintedImageValue(value: unknown): StructValue | undefined {
  if (typeof value === "string") return imageValueFromDigits(value);
  return isImageStructValue(value) ? value : undefined;
}

/**
 * Registers the `create an image` literal factory tile: each manufacture mints
 * an `Image` literal tile carrying the submitted image under a freshly minted
 * unique identity, so two manufactures of identical pixel content yield
 * distinct tiles. The image is named either by an `Image` struct value or by a
 * pixel-digit string (see {@link imageValueFromDigits}). The minted literal
 * persists into a saved brain. A manufacture whose `value` option names neither
 * mints nothing and returns `undefined`.
 */
function registerImageLiteralFactory(api: WendooModuleApi): void {
  const services = api.brainServices;
  api.registerTile(
    new BrainTileFactoryDef(
      mkLiteralFactoryTileId(WODAL_IMAGE_LITERAL_FACTORY_ID),
      WODAL_IMAGE_LITERAL_FACTORY_ID,
      (factoryTileDef, opts) => {
        const value = mintedImageValue(opts.value);
        if (value === undefined) return undefined;
        return new BrainTileLiteralDef(
          factoryTileDef.producedDataType,
          value,
          { uniqueId: mintDocumentId(services.app.rng) },
          services
        );
      },
      WODAL_SHARED_TYPE_IDS.Image,
      { metadata: { label: "create an image" } }
    )
  );
}
