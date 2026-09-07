import react from "@vitejs/plugin-react";
import path from "path";
import { defineConfig } from "vite";
import { uiPlugin } from "../../../external/wendoo-lang/packages/ui/src/vite-plugin.ts";
import { embeddedExtensions } from "./embedded-extensions.mjs";

export default defineConfig({
  // Absolute base for the standalone web build (`npm run build`), matching
  // apps/ecosim: it deploys to the S3 bucket root at microbit.playwendoo.com,
  // where a relative base breaks a hard-loaded deep route -- its assets resolve
  // against the route path, so a load of /docs/x/y requests /docs/x/assets/...
  // and misses. The portable target-package build (`npm run package`) overrides
  // this with `--base=./` for the VS Code webview, which hosts the app from a
  // non-root resource path.
  base: "/",
  plugins: [react(), uiPlugin(), embeddedExtensions()],
  resolve: {
    alias: {
      "@": path.resolve(process.cwd(), "./src"),
      "@wendoo/assistant-panel": path.resolve(
        process.cwd(),
        "../../external/wendoo-lang/packages/assistant-panel/src"
      ),
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
