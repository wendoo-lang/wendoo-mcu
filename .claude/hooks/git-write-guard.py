#!/usr/bin/env python3
"""PreToolUse:Bash: block git commands that MUTATE the repo; allow read-only git.

Git is read-only for agents (AGENTS.md, "Git Is Read-Only"). This gate enforces
that mechanically: every git invocation in the Bash command is parsed, its
subcommand resolved (skipping global options like `-C <path>`), and blocked if it
writes. Reads pass untouched -- the priority is that inspection and history
queries (status/log/diff/show/blame/reflog/...) never break.

Policy:
  - default ALLOW; only a known write set blocks (so no read is ever caught).
  - `rm` and `mv` are allowed (natural file operations when editing the repo).
  - dual verbs (stash/branch/tag/config/remote/worktree/submodule/notes/reflog/
    symbolic-ref/bisect) are inspected: the listing/read form passes, the
    mutating form blocks.
  - OVERRIDE: if the command contains the token `allow-git-write`, it passes.
    This is the explicit per-turn user authorization path (AGENTS.md permits a
    git write when the user instructs it in that turn). Do not add the override
    on your own initiative.

Blocks by exiting 2 with a message on stderr (the seed-gate convention). Fails
OPEN on any parse error or non-git command -- a guardrail must never wedge the
session; the AGENTS.md text rule and the kickoff clause are the backstop.
"""

import json
import os
import re
import shlex
import sys

OVERRIDE_TOKEN = "allow-git-write"

# Subcommands that always mutate the repo/index/working tree/refs. `rm` and `mv`
# are deliberately absent (allowed). `cherry` is absent (it is a read, unlike
# `cherry-pick`).
BLOCK_ALWAYS = {
    "add", "commit", "commit-tree", "push", "pull", "fetch", "merge",
    "rebase", "reset", "revert", "cherry-pick", "checkout", "switch", "restore",
    "clean", "apply", "am", "gc", "prune", "repack", "pack-refs",
    "filter-branch", "filter-repo", "update-index", "update-ref", "write-tree",
    "fast-import", "mergetool", "rerere", "quiltimport",
}

# Git global options consuming a following value token; used to locate the real
# subcommand (the incident was `git -C <path> checkout`).
GLOBAL_WITH_VALUE = {
    "-C", "-c", "--git-dir", "--work-tree", "--namespace", "--super-prefix",
    "--config-env", "--exec-path",
}
GLOBAL_EQ_PREFIXES = (
    "--git-dir=", "--work-tree=", "--namespace=", "--super-prefix=",
    "--config-env=", "--exec-path=", "-c",
)

WRAPPERS = {
    "sudo", "env", "command", "nice", "time", "builtin", "exec", "then", "do",
    "else", "!", "\\", "xargs",
}


def _positionals(args):
    return [a for a in args if not a.startswith("-")]


def _first_subverb(args):
    for a in args:
        if not a.startswith("-"):
            return a
    return ""


def dual_stash(args):
    return _first_subverb(args) not in ("list", "show")


def dual_branch(args):
    write_flags = {
        "-d", "-D", "--delete", "-m", "-M", "--move", "-c", "-C", "--copy",
        "-f", "--force", "--set-upstream-to", "-u", "--unset-upstream",
        "--edit-description",
    }
    read_flags = {
        "--list", "-l", "-a", "--all", "-r", "--remotes", "-v", "-vv",
        "--verbose", "--contains", "--no-contains", "--merged", "--no-merged",
        "--points-at", "--show-current", "--format", "--sort", "--color",
        "--no-color", "--column", "--no-column",
    }
    if any(f in args for f in write_flags):
        return True
    has_read = any(a in read_flags or a.split("=", 1)[0] in read_flags for a in args)
    return bool(_positionals(args)) and not has_read


def dual_tag(args):
    write_flags = {
        "-d", "--delete", "-a", "--annotate", "-s", "--sign", "-m", "--message",
        "-F", "--file", "-f", "--force", "-e", "--edit", "--create-reflog",
    }
    read_flags = {
        "-l", "--list", "-n", "--contains", "--no-contains", "--merged",
        "--no-merged", "--points-at", "--format", "--sort", "--column",
        "--color", "-i", "--ignore-case",
    }
    if any(f in args for f in write_flags):
        return True
    has_read = any(a in read_flags or a.split("=", 1)[0] in read_flags for a in args)
    return bool(_positionals(args)) and not has_read


def dual_config(args):
    write_flags = {
        "--add", "--unset", "--unset-all", "--replace-all", "--rename-section",
        "--remove-section", "--edit", "-e",
    }
    if any(f in args for f in write_flags):
        return True
    read_flags = {
        "--get", "--get-all", "--get-regexp", "--get-urlmatch", "-l", "--list",
        "--name-only", "--show-origin", "--show-scope", "-z", "--null",
    }
    has_read = any(a in read_flags or a.split("=", 1)[0] in read_flags for a in args)
    return len(_positionals(args)) >= 2 and not has_read


def dual_remote(args):
    return _first_subverb(args) in {
        "add", "remove", "rm", "rename", "set-url", "set-head", "set-branches",
        "prune", "update",
    }


def dual_worktree(args):
    return _first_subverb(args) in {
        "add", "remove", "move", "prune", "lock", "unlock", "repair",
    }


# The one foreach form that is a read: running `git status` in every submodule
# (the CI clean-tree assert). Any other foreach command still blocks.
FOREACH_STATUS = re.compile(r"^git\s+status(\s|$)")


def _foreach_is_status_read(args):
    rest = list(args)
    while rest and rest[0].startswith("-"):
        rest.pop(0)
    if not rest or rest[0] != "foreach":
        return False
    command = [a for a in rest[1:] if a not in ("--recursive", "-q", "--quiet")]
    return bool(command) and bool(FOREACH_STATUS.match(" ".join(command)))


def dual_submodule(args):
    if _first_subverb(args) == "foreach":
        return not _foreach_is_status_read(args)
    return _first_subverb(args) in {
        "add", "update", "init", "deinit", "set-url", "set-branch", "sync",
        "absorbgitdirs",
    }


def dual_notes(args):
    return _first_subverb(args) in {
        "add", "append", "copy", "edit", "remove", "prune", "merge",
    }


def dual_reflog(args):
    return _first_subverb(args) in {"expire", "delete", "drop"}


def dual_symbolic_ref(args):
    if "-d" in args or "--delete" in args:
        return True
    return len(_positionals(args)) >= 2


def dual_bisect(args):
    return _first_subverb(args) in {
        "start", "good", "bad", "new", "old", "skip", "reset", "run", "replay",
    }


DUALS = {
    "stash": dual_stash, "branch": dual_branch, "tag": dual_tag,
    "config": dual_config, "remote": dual_remote, "worktree": dual_worktree,
    "submodule": dual_submodule, "notes": dual_notes, "reflog": dual_reflog,
    "symbolic-ref": dual_symbolic_ref, "bisect": dual_bisect,
}


def is_write(subcommand, args):
    if "-h" in args or "--help" in args:
        return False
    if subcommand in BLOCK_ALWAYS:
        return True
    handler = DUALS.get(subcommand)
    if handler:
        return handler(args)
    return False


def _segments(command):
    """Every simple-command fragment: split on shell operators, and also pull the
    inner text of $(...) and `...` substitutions so a nested git write is seen."""
    inner = re.findall(r"\$\(([^()]*(?:\([^()]*\)[^()]*)*)\)", command)
    inner += re.findall(r"`([^`]*)`", command)
    pieces = [command] + inner
    out = []
    for piece in pieces:
        out.extend(re.split(r"\|\||&&|[;\n|&()<>]", piece))
    return out


# Interpreters that run a command string passed via a `-c`/`-lc`/`-ic` flag; the
# inner string is scanned as its own command so `zsh -c "git checkout ..."` and
# friends do not slip past.
SHELLS = {
    "sh", "bash", "zsh", "dash", "ksh", "ksh93", "mksh", "ash", "fish", "csh",
    "tcsh",
}
SHELL_C_FLAG = re.compile(r"^-[A-Za-z]*c$")


def _git_subcommand(tokens, i):
    """(subcommand, args) for the git invocation starting at token i, past global
    options like `-C <path>`; (None, None) for a bare `git` (a usage read)."""
    j = i + 1
    while j < len(tokens):
        t = tokens[j]
        if t in GLOBAL_WITH_VALUE:
            j += 2
            continue
        if any(t.startswith(p + "=") or (p != "-c" and t.startswith(p)) for p in GLOBAL_EQ_PREFIXES):
            j += 1
            continue
        if t.startswith("-"):
            j += 1
            continue
        break
    if j >= len(tokens):
        return None, None
    return tokens[j], tokens[j + 1:]


def find_git_write(command, depth=0):
    """The subcommand of the first git write in `command`, else None. Recurses
    into `<shell> -c <string>` and `eval <string>` so a nested invocation is seen."""
    if depth > 6:
        return None
    for segment in _segments(command):
        try:
            tokens = shlex.split(segment, comments=True)
        except ValueError:
            tokens = segment.split()
        if not tokens:
            continue

        i = 0
        while i < len(tokens):
            t = tokens[i]
            if re.match(r"^[A-Za-z_][A-Za-z0-9_]*=", t) or t in WRAPPERS:
                i += 1
                continue
            break
        if i >= len(tokens):
            continue

        prog = os.path.basename(tokens[i].lstrip("\\"))
        rest = tokens[i + 1:]

        if prog == "eval":
            hit = find_git_write(" ".join(rest), depth + 1)
            if hit:
                return hit
            continue
        if prog in SHELLS:
            for k, flag in enumerate(rest):
                if SHELL_C_FLAG.match(flag) and k + 1 < len(rest):
                    hit = find_git_write(rest[k + 1], depth + 1)
                    if hit:
                        return hit
                    break
            continue
        if prog == "git":
            subcommand, args = _git_subcommand(tokens, i)
            if subcommand is not None and is_write(subcommand, args):
                return subcommand

    return None


def main():
    try:
        payload = json.load(sys.stdin)
    except (json.JSONDecodeError, ValueError):
        sys.exit(0)

    if payload.get("tool_name") != "Bash":
        sys.exit(0)
    command = payload.get("tool_input", {}).get("command")
    if not isinstance(command, str) or not re.search(r"\bgit\b", command):
        sys.exit(0)
    if OVERRIDE_TOKEN in command:
        sys.exit(0)

    offending = find_git_write(command)
    if offending:
            sys.stderr.write(
                "Git is read-only for agents (AGENTS.md, 'Git Is Read-Only'). "
                f"Blocked git write: `git {offending}`.\n"
                "Read-only git is fine -- status, log, diff, show, blame, "
                "reflog, and other history queries.\n"
                "If the user explicitly authorized THIS write in this turn, "
                f"re-run with the token `{OVERRIDE_TOKEN}` in the command "
                f"(e.g. append `# {OVERRIDE_TOKEN}`). Never add the override on "
                "your own initiative.\n"
            )
            sys.exit(2)

    sys.exit(0)


if __name__ == "__main__":
    main()
