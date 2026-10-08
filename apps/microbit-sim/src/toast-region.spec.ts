/**
 * Pins that both views the page can mount -- the simulator app and the
 * standalone docs page -- render the shared toast region, so a toast the app
 * or the shared components raise is shown on either.
 */

import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { describe, test } from "node:test";
import { fileURLToPath } from "node:url";

/** The source of the view module at `relativePath`, beside this spec. */
function viewSource(relativePath: string): string {
  return readFileSync(fileURLToPath(new URL(relativePath, import.meta.url)), "utf8");
}

describe("the toast region", () => {
  for (const view of ["./App.tsx", "./DocsPage.tsx"]) {
    test(`is rendered by ${view}, from @wendoo/ui`, () => {
      const source = viewSource(view);
      assert.match(source, /import \{[^}]*\bToaster\b[^}]*\} from "@wendoo\/ui";/);
      assert.match(source, /<Toaster \/>/);
    });
  }
});
