import react from "@vitejs/plugin-react";
import path from "path";
import { defineConfig } from "vite";
import { uiPlugin } from "../../../external/wendoo-lang/packages/ui/src/vite-plugin.ts";
import { embeddedExtensions } from "./embedded-extensions.mjs";

export default defineConfig({
  base: "/",
  appType: "spa",
  plugins: [react(), uiPlugin(), embeddedExtensions()],
  resolve: {
    alias: {
      "@": path.resolve(process.cwd(), "./src"),
      "@wendoo/assistant-panel": path.resolve(process.cwd(), "../../external/wendoo-lang/packages/assistant-panel/src"),
      "@wendoo/ui": path.resolve(process.cwd(), "../../external/wendoo-lang/packages/ui/src"),
      "@wendoo/docs": path.resolve(process.cwd(), "../../external/wendoo-lang/packages/docs/src"),
    },
  },
  optimizeDeps: {
    // @wendoo/ui is not listed here: it is aliased to source, so it is
    // never prebundled anyway, and excluding it stops the dep scanner from
    // walking into it. Its Radix imports would then be discovered only on the
    // first page request, mid-load, and the modules served before that pass
    // finishes get a second React instance.
    exclude: ["@wendoo/core"],
  },
  server: {
    fs: {
      allow: [path.resolve(process.cwd(), "../..")],
    },
  },
});
