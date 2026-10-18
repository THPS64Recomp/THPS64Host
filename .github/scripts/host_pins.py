#!/usr/bin/env python3
"""Which THPS64Host release each game's main branch pins, and one drift issue that stays up to date (host-pins.yml).

    host_pins.py [--org ORG] [--games "R1 R2 R3"] [--max-days N] [--host-repo DIR] [--dry-run] [--pins-json FILE] [--now ISO]
                 [--open-issue N]

For every game: the gitlink at decomp/common on main (GitHub contents API), and the newest annotated vX.Y.Z tag of this host
repository that names it.
  * missing   no submodule at decomp/common             -> the issue is opened at once
  * untagged  the pin is not a host release tag         -> the issue is opened at once
  * lagging   the pin is older than the newest tag any game pins; its age is the time since the FIRST host release after
              its own tag (when it first fell behind). That date doesn't move when newer tags appear, so a game that stays
              behind is reported after --max-days, however often the others bump.
One issue, titled TITLE, is created when an alert is due, its body is rewritten on every run while any drift remains, and it is
closed with a comment once every game is on the same release tag. Reads use READ_TOKEN (falls back to GH_TOKEN); issue writes
use GH_TOKEN. Standard library + the gh CLI. Tests: --pins-json {"repo": "sha" | null}, --dry-run (no GitHub calls) and
--open-issue N (with --dry-run: act as if drift issue #N were open).
"""
import datetime as dt
import json
import os
import re
import subprocess
import sys

TITLE = "Host pin drift: a game is behind or off a THPS64Host release"
SEMVER = re.compile(r"^v(\d+)\.(\d+)\.(\d+)$")


def key(tag):
    return tuple(int(x) for x in SEMVER.match(tag).groups())


def fail(title, msg):
    if os.environ.get("GITHUB_ACTIONS"):
        print(f"::error title={title}::{msg}")
    else:
        print(f"error: {title}: {msg}", file=sys.stderr)
    sys.exit(1)


def gh(args, token_env, check=True):
    env = dict(os.environ)
    tok = os.environ.get(token_env) or os.environ.get("GH_TOKEN")
    if tok:
        env["GH_TOKEN"] = tok
    r = subprocess.run(["gh"] + args, capture_output=True, text=True, env=env)
    if check and r.returncode != 0:
        fail("GitHub call failed", f"gh {' '.join(args[:3])} failed: {r.stderr.strip()[:300]}")
    return r


def host_tags(repo_dir):
    """{tag: (commit, date)} for the annotated semver tags of the host checkout."""
    out = subprocess.run(["git", "-C", repo_dir, "for-each-ref", "refs/tags",
                          "--format=%(refname:short) %(objecttype) %(*objectname) %(taggerdate:iso-strict)"],
                         capture_output=True, text=True, check=True).stdout
    tags = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[1] == "tag" and SEMVER.match(parts[0]):
            tags[parts[0]] = (parts[2], dt.datetime.fromisoformat(parts[3]))
    if not tags:
        fail("No host release tags", f"{repo_dir} has no annotated vX.Y.Z tags. The workflow checks out with fetch-depth: 0 to see them; "
             "if it did, tag a host release first (RELEASING.md).")
    return tags


def game_pin(org, repo):
    r = gh(["api", f"repos/{org}/{repo}/contents/decomp/common?ref=main"], "READ_TOKEN", check=False)
    if r.returncode != 0:
        if "Not Found" in r.stderr or "404" in r.stderr:
            return None
        fail("Cannot read a game repository",
             f"Reading {org}/{repo} failed ({r.stderr.strip()[:200]}). While the repositories are private this needs the "
             "THPS64_READ_TOKEN secret (RELEASING.md); otherwise retry the run.")
    data = json.loads(r.stdout)
    return data.get("sha") if isinstance(data, dict) and data.get("type") == "submodule" else None


def main(argv):
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--org", default=os.environ.get("ORG", "THPS64Recomp"))
    ap.add_argument("--games", default=os.environ.get("GAMES", "THPS1-64Recomp THPS2-64Recomp THPS3-64Recomp"))
    ap.add_argument("--max-days", type=int, default=int(os.environ.get("MAX_DAYS", "30")))
    ap.add_argument("--host-repo", default=".")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--pins-json")
    ap.add_argument("--now")
    ap.add_argument("--open-issue", type=int, help="tests, with --dry-run: act as if this drift issue were open")
    a = ap.parse_args(argv)
    now = dt.datetime.fromisoformat(a.now) if a.now else dt.datetime.now(dt.timezone.utc)
    tags = host_tags(a.host_repo)
    by_commit = {}
    for t, (c, _) in tags.items():
        by_commit.setdefault(c, []).append(t)
    fake = json.load(open(a.pins_json)) if a.pins_json else None

    rows = []          # (repo, state, tag, sha, since, days)
    for repo in a.games.split():
        sha = fake.get(repo) if fake is not None else game_pin(a.org, repo)
        if not sha:
            rows.append([repo, "missing", None, None, None, None])
        elif sha not in by_commit:
            rows.append([repo, "untagged", None, sha, None, None])
        else:
            rows.append([repo, "ok", max(by_commit[sha], key=key), sha, None, None])
    pinned = [r[2] for r in rows if r[2]]
    newest = max(pinned, key=key) if pinned else None
    for r in rows:
        if r[1] == "ok" and r[2] != newest:
            later = [t for t in tags if key(t) > key(r[2])]
            first = min(later, key=key)
            r[1], r[4] = "lagging", tags[first][1]
            r[5] = (now - r[4]).days

    table = ["| Game | State | Host tag | Commit | Behind since |", "|---|---|---|---|---|"]
    for repo, state, tag, sha, since, days in rows:
        table.append(f"| {repo} | {state} | {tag or '-'} | {('`' + sha[:12] + '`') if sha else '-'} | "
                     f"{f'{since.date()} ({days} days)' if since else '-'} |")
    summary = "\n".join(table)
    print(summary)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as f:
            f.write(summary + "\n")

    bad = [r for r in rows if r[1] in ("missing", "untagged")]
    lag = [r for r in rows if r[1] == "lagging"]
    alert = bool(bad) or any(r[5] > a.max_days for r in lag)
    drift = bool(bad or lag)

    def act(args, what):
        if a.dry_run:
            print(f"[dry-run] would {what}")
            return ""
        return gh(args, "GH_TOKEN").stdout

    fake_issue = json.dumps([{"number": a.open_issue, "title": TITLE}]) if a.open_issue else "[]"
    listing = fake_issue if a.dry_run else gh(["issue", "list", "--state", "open", "--search", f'in:title "{TITLE}"',
                                         "--json", "number,title", "--limit", "20"], "GH_TOKEN").stdout
    issue = next((i["number"] for i in json.loads(listing or "[]") if i["title"] == TITLE), None)

    if not drift:
        print(f"Every game pins THPS64Host {newest}.")
        if issue:
            act(["issue", "close", str(issue), "--comment", f"Resolved: every game now pins THPS64Host {newest}."],
                f"close issue #{issue}")
        return
    body = [f"<!-- written by host-pins.yml; rewritten on every run while drift remains, closed when it is gone -->",
            f"Checked {now.strftime('%Y-%m-%d %H:%M')} UTC. The newest host release a game pins is **{newest or 'none'}**; "
            f"a game more than {a.max_days} days behind it, or on no release at all, needs a bump.", "", summary, "", "**How to fix**"]
    for repo, state, tag, sha, since, days in rows:
        to = newest or max(tags, key=key)
        if state == "missing":
            body.append(f"- {repo}: main has no submodule at `decomp/common`. Restore it from the import commit (`.gitmodules` and the "
                        f"gitlink), pinned to {to}; its CI fails until then.")
        elif state == "untagged":
            body.append(f"- {repo}: `decomp/common` is at `{sha[:12]}`, which no host release tag names. Move it to a release: "
                        f"`git -C decomp/common checkout {to}`, `git add decomp/common`, and update the `@v...`/`host-tag:` lines "
                        f"in `.github/workflows/` to {to}, in one PR.")
        elif state == "lagging":
            body.append(f"- {repo}: on {tag}, behind {newest} since {since.date()} ({days} days). Open a small PR that moves "
                        f"`decomp/common` and the workflow pins to {newest}, with what the host CHANGELOG lists under \"Games must\" "
                        f"(playbook scenario C, steps 6-7).")
    text = "\n".join(body) + "\n"
    if issue:
        act(["issue", "edit", str(issue), "--body", text], f"update issue #{issue}")
        print(f"Drift remains; issue #{issue} updated.")
    elif alert:
        act(["issue", "create", "--title", TITLE, "--body", text], "create the drift issue")
        print("Drift past the limit (or a missing/untagged pin): issue opened.")
    else:
        print(f"The games differ, but none has been behind for more than {a.max_days} days yet.")
    if a.dry_run:
        print(text)


if __name__ == "__main__":
    main(sys.argv[1:])
