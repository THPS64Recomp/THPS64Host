#!/bin/sh
# Is this game repository's decomp/common an official THPS64Host release? The one check the game CI, the release workflow and the
# maintainer's import script share (it lives in THPS64Host, so it can't drift between the three games).
#
#   check_host_pin.sh [--repo DIR] [--expect-tag vX.Y.Z] [--remote URL]
#
# It checks, in order:
#   1. .gitmodules has exactly one submodule, at decomp/common, with the canonical URL and no branch line;
#   2. HEAD has a gitlink (mode 160000) at decomp/common;
#   3. the canonical host repository (asked directly with `git ls-remote`, never the URL in .gitmodules) has an ANNOTATED tag vX.Y.Z
#      whose commit is that gitlink;
#   4. with --expect-tag: that tag is the one the workflow itself is pinned to (`uses: ...@vX.Y.Z`), so both move together.
# It needs no submodule checkout. --remote replaces only where the tags are read from (tests on local repositories); the URL in
# .gitmodules must still be the canonical one. THPS64_HOST_TOKEN, if set, authenticates the ls-remote (private repositories).
# Every failure says what failed and how to fix it; in GitHub Actions it is an ::error annotation.
# The maintainers' pre-push guard applies rule 3 to every pushed commit the same way: `git ls-remote
# --tags` of a host checkout, annotated vX.Y.Z only. Change both together.
set -eu

CANONICAL=https://github.com/THPS64Recomp/THPS64Host.git
SUB=decomp/common
REPO=.
EXPECT=
REMOTE=$CANONICAL
while [ $# -gt 0 ]; do
    case "$1" in
        --repo) REPO=$2; shift 2 ;;
        --expect-tag) EXPECT=$2; shift 2 ;;
        --remote) REMOTE=$2; shift 2 ;;
        *) echo "usage: $0 [--repo DIR] [--expect-tag vX.Y.Z] [--remote URL]" >&2; exit 2 ;;
    esac
done

fail() {   # $1 = short title, $2 = what failed and how to fix it (one line)
    if [ -n "${GITHUB_ACTIONS:-}" ]; then echo "::error title=$1::$2"; else echo "error: $1: $2" >&2; fi
    exit 1
}
note() {
    echo "$1"
    if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then echo "$1" >> "$GITHUB_STEP_SUMMARY"; fi
}

[ -f "$REPO/.gitmodules" ] || fail "No .gitmodules" "This repository has no .gitmodules, so decomp/common is not a submodule. Restore .gitmodules from main (path $SUB, url $CANONICAL) and commit it."

# 1. exactly one submodule, at decomp/common, canonical URL, no branch
paths=$(git config -f "$REPO/.gitmodules" --get-regexp '^submodule\..*\.path$' | awk '{print $2}')
[ "$paths" = "$SUB" ] || fail "Unexpected submodules" "The game repository must have exactly one submodule, at $SUB (found: $(echo $paths | tr '\n' ' ')). Remove the others from .gitmodules."
name=$(git config -f "$REPO/.gitmodules" --get-regexp '^submodule\..*\.path$' | awk '{print $1}' | sed 's/^submodule\.//; s/\.path$//')
url=$(git config -f "$REPO/.gitmodules" "submodule.$name.url" || true)
[ "$url" = "$CANONICAL" ] || fail "Host submodule URL" "decomp/common points at '$url', not the official host. Run: git config -f .gitmodules submodule.$name.url $CANONICAL && git submodule sync, then commit .gitmodules."
if git config -f "$REPO/.gitmodules" "submodule.$name.branch" > /dev/null; then
    fail "Host submodule branch" "The .gitmodules entry for decomp/common names a branch; games pin a host release tag. Run: git config -f .gitmodules --unset submodule.$name.branch, then commit .gitmodules."
fi

# 2. the gitlink
line=$(git -C "$REPO" ls-tree HEAD "$SUB")
mode=$(echo "$line" | awk '{print $1}')
pin=$(echo "$line" | awk '{print $3}')
[ "$mode" = 160000 ] || fail "Host submodule missing" "HEAD has no submodule commit at $SUB (found '${line:-nothing}'). Run: git submodule update --init, then git -C $SUB checkout vX.Y.Z and git add $SUB."

# 3. an annotated semver tag on the canonical host names that commit
if [ -n "${THPS64_HOST_TOKEN:-}" ]; then
    auth=$(printf 'x-access-token:%s' "$THPS64_HOST_TOKEN" | base64 | tr -d '\n')
    refs=$(git -c "http.https://github.com/.extraheader=AUTHORIZATION: basic $auth" ls-remote --tags "$REMOTE") \
        || fail "Host tags unreadable" "Could not read the tags of $REMOTE. While the repositories are private this needs the THPS64_READ_TOKEN secret (RELEASING.md); otherwise retry the run."
else
    refs=$(git ls-remote --tags "$REMOTE") \
        || fail "Host tags unreadable" "Could not read the tags of $REMOTE. If the host repository is private, add the THPS64_READ_TOKEN secret (RELEASING.md); otherwise retry the run."
fi
# annotated: the peeled line "<commit> refs/tags/vX.Y.Z^{}"; a lightweight tag has no ^{} line
annotated=$(echo "$refs" | awk -v p="$pin" '$1 == p && $2 ~ /\^\{\}$/ {t=$2; sub(/^refs\/tags\//, "", t); sub(/\^\{\}$/, "", t); print t}' \
            | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | sort -V || true)
if [ -z "$annotated" ]; then
    light=$(echo "$refs" | awk -v p="$pin" '$1 == p && $2 !~ /\^\{\}$/ {t=$2; sub(/^refs\/tags\//, "", t); print t}' | tr '\n' ' ')
    latest=$(echo "$refs" | awk '$2 ~ /\^\{\}$/ {t=$2; sub(/^refs\/tags\//, "", t); sub(/\^\{\}$/, "", t); print t}' \
             | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | sort -V | tail -1 || true)
    hint="Run: git -C $SUB fetch --tags && git -C $SUB checkout ${latest:-vX.Y.Z} && git add $SUB, set the @v... and host-tag: lines in .github/workflows/ to the same tag, and commit both."
    if [ -n "$light" ]; then
        fail "Host pin not a release" "decomp/common is at $pin, which only a lightweight or non-release tag names ($light). Host releases are annotated vX.Y.Z tags. $hint"
    fi
    fail "Host pin not a release" "decomp/common is at $pin, which is not a THPS64Host release tag (vX.Y.Z on $CANONICAL). $hint"
fi
tag=$(echo "$annotated" | tail -1)

# 4. the workflow pin and the submodule agree
if [ -n "$EXPECT" ]; then
    echo "$annotated" | grep -qx "$EXPECT" || fail "Workflow and submodule differ" "decomp/common is at THPS64Host $tag, but the workflows call THPS64Host@$EXPECT. Change every 'THPS64Host/.github/workflows/...@' and 'host-tag:' in .github/workflows/ to $tag (or move decomp/common to $EXPECT), and commit both together."
fi
note "decomp/common is THPS64Host $tag ($pin), an annotated release tag on $CANONICAL."
