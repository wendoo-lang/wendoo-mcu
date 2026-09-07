export {
  buildWodalProgramImage,
  type WodalBuildDiagnostic,
  WodalBuildDiagnosticCode,
  type WodalBuildInput,
  type WodalBuildResult,
} from "./wendoo/build-kernel";
export {
  createWodalProgramImage,
  type WodalProgramImageCreateOptions,
} from "./wendoo/build-program-image";
export {
  getWodalDeviceProfile,
  isWodalDeviceProfileId,
  WODAL_DEVICE_PROFILE_IDS,
  WODAL_DEVICE_PROFILES,
  type WodalDeviceProfile,
  WodalDeviceProfileId,
} from "./wendoo/device-profile";
export { createWodalEnvironment } from "./wendoo/environment";
export type { FirmwareMetadata } from "./wendoo/firmware-metadata";
export {
  FIRMWARE_PATCH_PROGRAM_TOO_LARGE,
  type FirmwarePatchInput,
  type FirmwarePatchProgramTooLargeError,
  type FirmwarePatchResult,
  patchFirmwareHex,
} from "./wendoo/firmware-patcher";
export { mkImageStructValue } from "./wendoo/image-value";
export {
  parseWodalProgramImage,
  serializeWodalProgramImageJson,
  validateWodalProgramImage,
  type WodalProgramImage,
  type WodalProgramImageParseResult,
  WodalProgramImageValidationCode,
  type WodalProgramImageValidationError,
} from "./wendoo/program-image";
export { wodalProgramBytes } from "./wendoo/program-image-binary";
export {
  type WodalProgramLoadFailure,
  type WodalProgramLoadSuccess,
  type WodalProgramLoadValidation,
  WodalProgramLoadValidationCode,
  type WodalProgramLoadValidationError,
} from "./wendoo/program-load";
export {
  createWodalSharedModule,
  WODAL_IMAGE_LITERAL_FACTORY_ID,
  WODAL_SHARED_MODULE_ID,
} from "./wendoo/shared-module";
export { ImageField, WODAL_SHARED_TYPE_IDS, WodalSharedTypeAtomId } from "./wendoo/shared-type-ids";
