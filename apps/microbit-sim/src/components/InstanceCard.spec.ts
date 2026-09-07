import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { describe, test } from "node:test";
import { fileURLToPath } from "node:url";
import { brainSelectStateClass } from "./InstanceCard";

/**
 * Reads the InstanceCard source. The card renders its recovery controls behind a
 * Radix popover whose content is portalled and gated on open state, and the shared
 * ui popover pulls in a second React copy under the node test runner, so the card
 * cannot be server-rendered here. The gate wiring is asserted against the source
 * of each control, matching the surface-invariants source-structure convention.
 */
function instanceCardSource(): string {
  return readFileSync(fileURLToPath(new URL("./InstanceCard.tsx", import.meta.url)), "utf8");
}

/** Extracts the `<button>...</button>` block carrying the given test id. */
function buttonBlock(source: string, testId: string): string {
  const marker = `data-testid="${testId}"`;
  const markerIndex = source.indexOf(marker);
  assert.notEqual(markerIndex, -1, `expected a control with data-testid="${testId}"`);
  const open = source.lastIndexOf("<button", markerIndex);
  const close = source.indexOf("</button>", markerIndex);
  assert.ok(open !== -1 && close !== -1, `expected an enclosing button for "${testId}"`);
  return source.slice(open, close);
}

describe("InstanceCard recovery controls gate on brain assignment", () => {
  const source = instanceCardSource();

  // Reset and edit-brain are how a user recovers a brain that failed to build; a
  // failed flash keeps its brain association (flashedBrainId), so these must gate on
  // assignment, not on the program having loaded.
  for (const testId of ["instance-reset", "instance-edit-brain"]) {
    test(`${testId} is disabled by absence of an assigned brain`, () => {
      const block = buttonBlock(source, testId);
      assert.match(block, /disabled=\{!assigned\}/, `${testId} must gate on assigned`);
      assert.doesNotMatch(block, /disabled=\{!loaded\}/, `${testId} must not gate on loaded`);
    });
  }

  test("no control gates on a loaded program", () => {
    assert.doesNotMatch(source, /!loaded/, "recovery controls must not depend on a loaded program");
  });

  test("assignment derives from the retained brain id, which survives a failed flash", () => {
    assert.match(source, /const assigned = instance\.flashedBrainId != null/);
  });

  test("remove stays ungated: a device can be removed in any flash state", () => {
    const block = buttonBlock(source, "instance-remove");
    assert.doesNotMatch(block, /disabled=/, "remove must not carry a disabled gate");
  });
});

describe("the brain selector colors by the assigned brain's error status", () => {
  test("an assigned brain with errors carries the red destructive tokens", () => {
    const className = brainSelectStateClass(true, true);
    assert.match(className, /border-destructive/);
    assert.match(className, /text-destructive/);
    assert.doesNotMatch(className, /warning/);
  });

  test("an assigned, clean brain uses the plain foreground token, neither destructive nor warning", () => {
    const className = brainSelectStateClass(true, false);
    assert.match(className, /text-foreground/);
    assert.doesNotMatch(className, /destructive/);
    assert.doesNotMatch(className, /warning/);
  });

  test("an unassigned selector keeps the amber warning tokens and is never destructive", () => {
    const className = brainSelectStateClass(false, false);
    assert.match(className, /border-warning/);
    assert.match(className, /text-warning/);
    assert.doesNotMatch(className, /destructive/);
  });
});

describe("the brain selector reflects only error status and updates live", () => {
  const source = instanceCardSource();

  test("the card reads the brainHasErrors status boolean, not any diagnostic detail", () => {
    assert.match(source, /store\.brainHasErrors\(/, "the selector's red must derive from the brainHasErrors boolean");
  });

  test("the card subscribes to the brain-diagnostics revision so the color re-renders on change", () => {
    assert.match(
      source,
      /useSyncExternalStore\(\s*store\.subscribeToBrainDiagnostics,\s*store\.getBrainDiagnosticsRevision\s*\)/,
      "InstanceCard must subscribe to the brain-diagnostics revision, mirroring the brain-list badge"
    );
  });
});

describe("the actions popover closes when a gesture is picked or the device is reset", () => {
  const source = instanceCardSource();

  test("the popover is controlled by the card's open state", () => {
    assert.match(source, /<Popover open=\{actionsOpen\} onOpenChange=\{setActionsOpen\}>/);
  });

  test("the gesture picker's selection callback clears that open state", () => {
    assert.match(source, /<GesturePicker instance=\{instance\} onSelected=\{\(\) => setActionsOpen\(false\)\} \/>/);
  });

  test("the reset control clears that open state before resetting", () => {
    const block = buttonBlock(source, "instance-reset");
    assert.match(block, /setActionsOpen\(false\);\s*void store\.resetInstance\(instance\.id\)/);
  });
});
