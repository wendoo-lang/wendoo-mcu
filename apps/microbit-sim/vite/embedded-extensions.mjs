import { embeddedExtensionsVitePlugin } from "@wendoo/bridge-app/node";
import path from "path";

// The extensions microbit-sim offers, by coordinate and source directory. The
// file list of each comes from its own wendoo.json, so adding a file to an
// extension needs no change here.
const registrations = [
  {
    coordinate: "wendoo-lang/trg-microbit-v2",
    dir: path.resolve(process.cwd(), "./target-package"),
  },
  {
    coordinate: "wendoo-lang/lib-microbit-v2",
    dir: path.resolve(process.cwd(), "../../packages/wodal/targets/microbit-v2/lib"),
  },
  { coordinate: "wendoo-lang/lib-codal", dir: path.resolve(process.cwd(), "../../packages/wodal/lib") },
  {
    coordinate: "wendoo-lang/lib-core",
    dir: path.resolve(process.cwd(), "../../external/wendoo-lang/packages/core/lib"),
  },
];

export function embeddedExtensions() {
  return embeddedExtensionsVitePlugin(registrations);
}
