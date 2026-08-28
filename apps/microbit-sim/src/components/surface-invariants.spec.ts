import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { describe, test } from "node:test";
import { fileURLToPath } from "node:url";

function componentSource(name: string): string {
  return readFileSync(fileURLToPath(new URL(`./${name}`, import.meta.url)), "utf8");
}

/** Every index `pattern` matches in `source`. */
function indexesOf(source: string, pattern: RegExp): number[] {
  return [...source.matchAll(new RegExp(pattern, "g"))].map((match) => match.index ?? -1);
}

/**
 * Identifiers of brain-diagnostic DETAILS. Device surfaces may reflect a brain's
 * error STATUS -- a plain boolean such as `store.brainHasErrors(id)`, which names
 * none of these -- but must not reach into the diagnostic details these
 * identifiers expose. The toast path likewise carries no diagnostics. So none of
 * these may appear in those components.
 */
const BRAIN_DIAGNOSTIC_IDENTIFIERS = [
  /flashState\.errors/,
  /flash\.errors/,
  /FlashDiagnostic/,
  /typecheckResult/i,
  /BrainDiagnosticEntry/,
  /newProblems/,
  /resolvedProblems/,
];

const DEVICE_COMPONENTS = ["InstanceCard.tsx", "MicrobitDevice.tsx", "Simulator.tsx"];

describe("device surfaces consume no brain-diagnostic state", () => {
  for (const component of DEVICE_COMPONENTS) {
    test(`${component} references no brain-diagnostic identifier`, () => {
      const source = componentSource(component);
      for (const identifier of BRAIN_DIAGNOSTIC_IDENTIFIERS) {
        assert.doesNotMatch(source, identifier);
      }
    });
  }
});

/**
 * Diagnostic-payload accesses of the install report. The toast path presents
 * through the shared presenter, whose payloads carry no diagnostics, so the
 * component never reads the report's problem lists.
 */
const REPORT_DIAGNOSTIC_ACCESSES = [
  /outcome\.newProblems/,
  /outcome\.resolvedProblems/,
  /typecheckResult/i,
  /BrainDiagnosticEntry/,
  /FlashDiagnostic/,
];

describe("the extension toast path consumes no brain-diagnostic state", () => {
  test("ProjectHeader.tsx references no diagnostic payload of the install report", () => {
    const source = componentSource("ProjectHeader.tsx");
    for (const identifier of REPORT_DIAGNOSTIC_ACCESSES) {
      assert.doesNotMatch(source, identifier);
    }
  });
});

describe("the header menu gates Settings, not About, on chrome.showSettings", () => {
  test("the menu trigger renders unconditionally and only the Settings item is gated", () => {
    const source = componentSource("ProjectHeader.tsx");

    const gateMatches = [...source.matchAll(/chrome\.showSettings/g)];
    assert.strictEqual(gateMatches.length, 1, "expected exactly one chrome.showSettings reference");
    const gateIndex = gateMatches[0].index;
    assert.ok(gateIndex !== undefined);

    const triggerIndex = source.indexOf('data-testid="app-menu-button"');
    assert.ok(triggerIndex >= 0, 'expected a hamburger trigger with data-testid="app-menu-button"');
    assert.ok(triggerIndex < gateIndex, "the menu trigger must render before (outside) the showSettings gate");

    const gateCloseIndex = source.indexOf(")}", gateIndex);
    assert.ok(gateCloseIndex >= 0, "expected a closing )} for the showSettings conditional");

    const settingsButtonIndex = source.indexOf('data-testid="settings-button"');
    assert.ok(settingsButtonIndex >= 0, "expected a settings-button menu item");
    assert.ok(
      settingsButtonIndex > gateIndex && settingsButtonIndex < gateCloseIndex,
      "settings-button must render inside the showSettings gate"
    );

    const aboutButtonIndex = source.indexOf('data-testid="about-button"');
    assert.ok(aboutButtonIndex >= 0, "expected an about-button menu item");
    assert.ok(aboutButtonIndex > gateCloseIndex, "about-button must render outside (after) the showSettings gate");
  });
});

describe("the header menu exposes a Learn more submenu with external links", () => {
  test("the app-menu-button trigger is present and the submenu carries both link targets", () => {
    const source = componentSource("ProjectHeader.tsx");

    assert.ok(
      source.includes('data-testid="app-menu-button"'),
      'expected the hamburger trigger to carry data-testid="app-menu-button"'
    );

    assert.ok(source.includes('data-testid="homepage-link"'), "expected a homepage-link menu item");
    assert.ok(source.includes('data-testid="github-link"'), "expected a github-link menu item");

    assert.ok(source.includes('href="https://wendoo-lang.org"'), "expected the homepage link's exact href");
    assert.ok(
      source.includes('href="https://github.com/wendoo-lang/wendoo-lang"'),
      "expected the github link's exact href"
    );
  });
});

describe("only the person's own open of the assistant region takes the keyboard", () => {
  test("BrainEditor.tsx raises the count of the person's own opens nowhere but in the region's toggle", () => {
    const source = componentSource("BrainEditor.tsx");
    const [raise, ...furtherRaises] = indexesOf(source, /setAssistantOpensByPerson\(\(opens\)/);
    const toggle = source.indexOf("const toggleAssistant = useCallback(");
    const afterToggle = source.indexOf("}, [brainId, store]);", toggle);

    assert.ok(toggle >= 0, "the editor stands a toggle for the region");
    assert.ok(raise !== undefined, "the toggle raises the count");
    assert.deepEqual(furtherRaises, [], "nothing else raises it");
    assert.ok(raise > toggle && raise < afterToggle, "the raise stands in the toggle");
  });

  test("BrainEditor.tsx starts the count over where it restores the region, so a restored open takes no keyboard", () => {
    const source = componentSource("BrainEditor.tsx");
    const restore = source.indexOf("setIsAssistantOpen(store.isAssistantPanelOpen(brainId))");
    const [drop, ...furtherDrops] = indexesOf(source, /setAssistantOpensByPerson\(undefined\)/);

    assert.ok(restore >= 0, "the editor restores where the region stood for the brain it opens on");
    assert.ok(drop !== undefined, "the count is started over");
    assert.deepEqual(furtherDrops, [], "in one place");
    assert.match(source.slice(restore), /^setIsAssistantOpen\([^\n]*\n\s*setAssistantOpensByPerson\(undefined\);/);
  });

  test("BrainEditor.tsx hands the count to the region's tenant", () => {
    assert.match(componentSource("BrainEditor.tsx"), /opensByPerson=\{assistantOpensByPerson\}/);
  });

  test("AssistantSidePanel.tsx hands the count on to the conversation surface", () => {
    assert.match(componentSource("AssistantSidePanel.tsx"), /opensByPerson=\{opensByPerson\}/);
  });
});

describe("the brain editor cannot undo a brain's creation", () => {
  test("BrainEditor.tsx reaches the store only to load and to save", () => {
    const source = componentSource("BrainEditor.tsx");

    assert.ok(source.includes("store.getBrain("), "expected the editor to load the brain it edits");
    assert.ok(source.includes("store.saveBrain("), "expected the editor to save on submit");

    assert.doesNotMatch(source, /removeBrain/);
  });
});

describe("the brain the assistant panel draws and navigates against", () => {
  const panelSource = componentSource("AssistantSidePanel.tsx");

  test("AssistantSidePanel.tsx builds the places from the brain the editor stands", () => {
    assert.match(panelSource, /brainPlacesOf\(edited, workspaces\)/);
  });

  test("AssistantSidePanel.tsx hands those places to the conversation surface", () => {
    assert.match(panelSource, /brainPlaces=\{brainPlaces\}/);
  });
});
