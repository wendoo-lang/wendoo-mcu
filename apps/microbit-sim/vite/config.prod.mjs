import react from "@vitejs/plugin-react";
import path from "path";
import { defineConfig } from "vite";
import { uiPlugin } from "../../../external/wendoo-lang/packages/ui/src/vite-plugin.ts";
import { embeddedExtensions } from "./embedded-extensions.mjs";

export default defineConfig({
  base: "./",
  plugins: [react(), uiPlugin(), embeddedExtensions()],
  resolve: {
    alias: {
      "@": path.resolve(process.cwd(), "./src"),
      "@wendoo/assistant-panel": path.resolve(process.cwd(), "../../external/wendoo-lang/packages/assistant-panel/src"),
      "@wendoo/ui": path.resolve(process.cwd(), "../../external/wendoo-lang/packages/ui/src"),
      "@wendoo/docs": path.resolve(process.cwd(), "../../external/wendoo-lang/packages/docs/src"),
    },
  },
  logLevel: "warning",
  build: {
    outDir: "dist",
    emptyOutDir: true,
  },
});
