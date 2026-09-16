#!/usr/bin/env python3
"""Blocking Stop guard: a turn that ADDS exported declarations must state each
new export's owner package before it ends.

Reads the Stop/SubagentStop hook JSON on stdin, scopes to files the current
agent edited in THIS turn (via the shared transcript helper), and scans each
file's uncommitted diff for added export DECLARATIONS (const/let/function/
class/interface/type/enum -- bare `export {...}`/`export *` re-exports are
skipped as barrel noise). When new declarations exist that this session has
not yet judged, it exits 2 (blocking the stop) with a prompt requiring, per
export, the owner package and a one-sentence ownership argument.

Judged exports are remembered per session in a temp ledger keyed by
(file, export name), so re-editing a file with already-judged uncommitted
exports does not re-prompt. Honors `stop_hook_active` so the gate cannot
wedge a session, and fails open whenever turn scope or git state cannot be
determined.
"""

import json
import os
import re
import subprocess
import sys
import tempfile

# Short-lived hook: skip writing a __pycache__ for the shared-helper import below.
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))
from _hook_turn_scope import current_turn_edited_paths

SOURCE_EXTENSIONS = (".ts", ".tsx", ".mts", ".js", ".mjs", ".cjs")

EXPORT_DECLARATION = re.compile(
    r"^\+\s*export\s+(?:default\s+)?(?:abstract\s+)?"
    r"(?:async\s+)?(const|let|var|function|class|interface|type|enum)\s+"
    r"([A-Za-z_$][A-Za-z0-9_$]*)"
)


def project_dir():
    return os.environ.get("CLAUDE_PROJECT_DIR") or os.getcwd()


def repo_for(path):
    """The git working-tree root containing `path`, or None."""
    try:
        out = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],
            cwd=os.path.dirname(path),
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError:
        return None
    top = out.stdout.strip()
    return top or None


def added_export_declarations(path):
    """(kind, name) pairs of export declarations the uncommitted diff ADDS to
    `path`. An untracked file contributes every export declaration it holds."""
    repo = repo_for(path)
    if repo is None:
        return []
    rel = os.path.relpath(path, repo)
    tracked = (
        subprocess.run(
            ["git", "ls-files", "--error-unmatch", rel],
            cwd=repo,
            capture_output=True,
            check=False,
        ).returncode
        == 0
    )
    if tracked:
        diff = subprocess.run(
            ["git", "diff", "HEAD", "--unified=0", "--", rel],
            cwd=repo,
            capture_output=True,
            text=True,
            check=False,
        ).stdout
        lines = diff.splitlines()
    else:
        try:
            with open(path, encoding="utf-8", errors="replace") as handle:
                lines = ["+" + line.rstrip("\n") for line in handle]
        except OSError:
            return []
    found = []
    for line in lines:
        match = EXPORT_DECLARATION.match(line)
        if match:
            found.append((match.group(1), match.group(2)))
    return found


def judged_ledger_path(payload):
    session = payload.get("session_id")
    if not isinstance(session, str) or not re.fullmatch(r"[A-Za-z0-9-]{4,64}", session):
        return None
    return os.path.join(tempfile.gettempdir(), f"claude-ownership-gate-{session}.json")


def read_judged(ledger):
    if ledger is None or not os.path.isfile(ledger):
        return set()
    try:
        with open(ledger, encoding="utf-8") as handle:
            entries = json.load(handle)
        return {tuple(entry) for entry in entries if isinstance(entry, list)}
    except (OSError, ValueError):
        return set()


def write_judged(ledger, judged):
    if ledger is None:
        return
    try:
        with open(ledger, "w", encoding="utf-8") as handle:
            json.dump(sorted([list(entry) for entry in judged]), handle)
    except OSError:
        pass


def main():
    try:
        payload = json.load(sys.stdin)
    except (json.JSONDecodeError, ValueError):
        payload = {}

    # A gate that already blocked this turn must not wedge the session.
    if payload.get("stop_hook_active"):
        sys.exit(0)

    root = project_dir()
    owned = current_turn_edited_paths(payload)
    if not owned:
        sys.exit(0)

    ledger = judged_ledger_path(payload)
    judged = read_judged(ledger)

    pending = {}  # display path -> [(kind, name), ...]
    newly_seen = set(judged)
    for path in sorted(owned):
        if not path.endswith(SOURCE_EXTENSIONS) or path.endswith(
            (".spec.ts", ".spec.js", ".spec.mjs")
        ):
            continue
        if "node_modules" in path.split(os.sep) or not os.path.isfile(path):
            continue
        display = os.path.relpath(path, root)
        for kind, name in added_export_declarations(path):
            key = (display, name)
            if key in judged:
                continue
            pending.setdefault(display, []).append((kind, name))
            newly_seen.add(key)

    if not pending:
        sys.exit(0)

    # Remember these as judged: the block below forces the statement now, and
    # re-editing the same file later in the session must not re-prompt for them.
    write_judged(ledger, newly_seen)

    listing = "\n".join(
        f"  {display}: " + ", ".join(f"{name} ({kind})" for kind, name in exports)
        for display, exports in sorted(pending.items())
    )
    sys.stderr.write(
        "Ownership gate: this turn ADDS exported declarations. Before ending, "
        "state for EACH new export below its OWNER package and a one-sentence "
        "ownership argument, per the Right-Layer Placement doctrine "
        "(.github/instructions/agent-posture.instructions.md): the symbol is "
        "named for the concept (never a consumer or transport), its producer "
        "lives with the concept's owner, and dependency edges point consumer "
        "-> owner. A placement that fails any of these is fixed now or "
        "reported as a finding with a proposed time -- not left silent.\n\n"
        "New exported declarations this turn:\n" + listing + "\n"
    )
    sys.exit(2)


if __name__ == "__main__":
    main()
