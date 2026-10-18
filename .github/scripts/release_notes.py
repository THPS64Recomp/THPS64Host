#!/usr/bin/env python3
"""The text of a GitHub release, from the matching section of CHANGELOG.md (Keep a Changelog).

    release_notes.py CHANGELOG.md VERSION [--out FILE] [--host-tag vX.Y.Z] [--allow-undated]

VERSION is the release version without the "v" (0.1.0, 0.1.0-preview). The section is `## [VERSION] - YYYY-MM-DD` and runs to
the next `## ` heading or the link definitions at the end. HTML comments are dropped. It fails, saying what to fix, when the
section is missing, empty, still dated "YYYY-MM-DD", or holds a TODO marker. The repositories carry no separate notes file: the
CHANGELOG section is the source, and the maintainer edits the draft on GitHub before publishing (RELEASING.md).
Standard library only; run with python3 -I.
"""
import os
import re
import sys

USAGE = "usage: release_notes.py CHANGELOG.md VERSION [--out FILE] [--host-tag vX.Y.Z] [--allow-undated]"


def fail(title, msg):
    if os.environ.get("GITHUB_ACTIONS"):
        print(f"::error title={title}::{msg}")
    else:
        print(f"error: {title}: {msg}", file=sys.stderr)
    sys.exit(1)


def extract(text, version, allow_undated=False):
    lines = text.splitlines()
    head = re.compile(r"^## \[(?P<v>[^\]]+)\](?:\s*-\s*(?P<d>\S+))?\s*$")
    start, date = None, None
    found = []
    for i, line in enumerate(lines):
        m = head.match(line)
        if m:
            found.append(m.group("v"))
            if m.group("v") == version and start is None:
                start, date = i, m.group("d")
    if start is None:
        listed = ", ".join(v for v in found if v != "Unreleased") or "none"
        fail("No CHANGELOG section",
             f"CHANGELOG.md has no '## [{version}] - YYYY-MM-DD' section (versions listed: {listed}). Rename '## [Unreleased]' "
             f"to '## [{version}] - <today>' (keep an empty '## [Unreleased]' above it), commit, and dispatch the release again.")
    if not allow_undated and (not date or not re.fullmatch(r"\d{4}-\d{2}-\d{2}", date)):
        fail("CHANGELOG section undated",
             f"The '## [{version}]' heading has no release date (found '{date or 'nothing'}'). Write it as "
             f"'## [{version}] - YYYY-MM-DD' with the real date, commit, and dispatch the release again.")
    body = []
    for line in lines[start + 1:]:
        if line.startswith("## ") or re.match(r"^\[[^\]]+\]:\s", line):
            break
        body.append(line)
    text = "\n".join(body)
    if re.search(r"<!--.*?\bTODO\b.*?-->", text, flags=re.S):
        fail("CHANGELOG section has a TODO",
             f"The '## [{version}]' section still has a TODO comment. Resolve it (and delete the comment), commit, and dispatch again.")
    text = re.sub(r"<!--.*?-->\n?", "", text, flags=re.S).strip()
    if not text.strip():
        fail("CHANGELOG section empty",
             f"The '## [{version}]' section of CHANGELOG.md is empty. List the changes (Added / Fixed / ...), commit, and dispatch again.")
    return text, date


def main(argv):
    args, out, host_tag, allow = [], None, None, False
    it = iter(argv)
    for a in it:
        if a == "--out":
            out = next(it, None)
        elif a == "--host-tag":
            host_tag = next(it, None)
        elif a == "--allow-undated":
            allow = True
        elif a.startswith("-"):
            sys.exit(USAGE)
        else:
            args.append(a)
    if len(args) != 2:
        sys.exit(USAGE)
    path, version = args
    version = version[1:] if version.startswith("v") else version
    if not os.path.isfile(path):
        fail("No CHANGELOG.md", f"{path} does not exist. Every game repository keeps a CHANGELOG.md at its root; restore it from main.")
    with open(path, encoding="utf-8") as f:
        text, _date = extract(f.read(), version, allow)
    notes = text + "\n"
    if host_tag:
        notes += f"\nBuilt on THPS64Host {host_tag}.\n"
    notes += ("\nCheck a download against `SHA256SUMS` with `shasum -a 256 -c SHA256SUMS` (macOS) or "
              "`Get-FileHash` (Windows).\n")
    if out:
        with open(out, "w", encoding="utf-8") as f:
            f.write(notes)
        print(f"release notes for {version}: {len(notes.splitlines())} lines -> {out}")
    else:
        sys.stdout.write(notes)


if __name__ == "__main__":
    main(sys.argv[1:])
