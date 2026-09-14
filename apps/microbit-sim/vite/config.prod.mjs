import react from "@vitejs/plugin-react";
import { readTargetPackageVersion } from "@wendoo/app-host/tooling";
import { createClientBuild } from "@wendoo/core/tooling";
import path from "path";
import { defineConfig } from "vite";
import { uiPlugin } from "../../../external/wendoo-lang/packages/ui/src/vite-plugin.ts";
import { embeddedExtensions } from "./embedded-extensions.mjs";

const appDir = path.resolve(__dirname, "..");

export default defineConfig({
  base: "./",
  plugins: [react(), uiPlugin(), embeddedExtensions()],
  define: { CLIENT_BUILD: JSON.stringify(createClientBuild(appDir, readTargetPackageVersion(appDir))) },
  resolve: {
    alias: {
      "@": path.resolve(appDir, "./src"),
      "@wendoo/assistant-panel": path.resolve(appDir, "../../external/wendoo-lang/packages/assistant-panel/src"),
      "@wendoo/ui": path.resolve(appDir, "../../external/wendoo-lang/packages/ui/src"),
      "@wendoo/docs": path.resolve(appDir, "../../external/wendoo-lang/packages/docs/src"),
    },
  },
  logLevel: "warning",
  build: {
    outDir: "dist",
    emptyOutDir: true,
  },
});
