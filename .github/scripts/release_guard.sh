#!/bin/sh
# Bind a release to the commit it is built from (THPS64Host game-release workflow; RELEASING.md "Tags and drafts").
#
#   release_guard.sh --repo OWNER/NAME --tag vX.Y.Z --sha COMMIT [--assets "NAME-OR-GLOB ..."] [--replace-assets true|false]
#
# The rules (the same ones release_manifest.py verify applies):
#   * The workflow never creates, moves or deletes a tag. The maintainer pushes an ANNOTATED tag vX.Y.Z at the commit recorded in the
#     release manifest, before publishing (GitHub would otherwise make a lightweight tag at the draft's target on publish).
#   * If the tag already exists, it must name COMMIT, the commit this run builds.
#   * At most one release (draft) for the tag. A published one is never touched. A draft's target must be COMMIT: a run from another
#     commit fails instead of overwriting the assets.
#   * A draft that already holds one of --assets is only overwritten with --replace-assets true (same commit only; the zip's SHA-256 then
#     changes, so the release manifest must be updated and approved again).
# Prints "draft=yes|no" (and appends it to $GITHUB_OUTPUT when set). Read-only: it only calls the GitHub API (GH_TOKEN).
set -euf    # -f: --assets holds glob patterns, matched with case, never against files

REPO='' TAG='' SHA='' ASSETS='' REPLACE=no
while [ $# -gt 0 ]; do
    case "$1" in
        --repo) REPO=$2; shift 2 ;;
        --tag) TAG=$2; shift 2 ;;
        --sha) SHA=$2; shift 2 ;;
        --assets) ASSETS=$2; shift 2 ;;
        --replace-assets) case "$2" in true|yes) REPLACE=yes ;; false|no|"") REPLACE=no ;; *) echo "--replace-assets takes true or false" >&2; exit 2 ;; esac; shift 2 ;;
        *) echo "usage: $0 --repo OWNER/NAME --tag vX.Y.Z --sha COMMIT [--assets \"...\"] [--replace-assets true|false]" >&2; exit 2 ;;
    esac
done
[ -n "$REPO" ] && [ -n "$TAG" ] && [ -n "$SHA" ] || { echo "usage: $0 --repo OWNER/NAME --tag vX.Y.Z --sha COMMIT" >&2; exit 2; }

fail() {
    if [ -n "${GITHUB_ACTIONS:-}" ]; then echo "::error title=$1::$2"; else echo "error: $1: $2" >&2; fi
    exit 1
}
short=$(echo "$SHA" | cut -c1-12)

# ---- the tag
ref=$(gh api "repos/$REPO/git/matching-refs/tags/$TAG" -q ".[] | select(.ref == \"refs/tags/$TAG\") | \"\(.object.type) \(.object.sha)\"")
if [ -n "$ref" ]; then
    type=${ref%% *}
    obj=${ref#* }
    [ "$type" = tag ] || fail "Tag is lightweight" "$TAG exists but is a lightweight tag. Release tags are annotated: delete it (git push origin :refs/tags/$TAG), then git tag -a $TAG -m \"$TAG\" $short and push it."
    commit=$(gh api "repos/$REPO/git/tags/$obj" -q .object.sha)
    [ "$commit" = "$SHA" ] || fail "Tag names another commit" "$TAG already exists at $(echo "$commit" | cut -c1-12), but this run builds $short. Dispatch the release from the tag (Run workflow > Use workflow from > Tags > $TAG), or use a new version. Tags are never moved."
    echo "tag $TAG exists and names $short"
else
    echo "tag $TAG does not exist yet (the maintainer creates it at $short before publishing)"
fi

# ---- the release (drafts are listed only with push access: this job has contents: write)
rels=$(gh api --paginate "repos/$REPO/releases?per_page=100" -q ".[] | select(.tag_name == \"$TAG\") | \"\(.id) \(.draft) \(.target_commitish)\"")
n=$(printf '%s' "$rels" | grep -c . || true)
draft=no
if [ "$n" -gt 1 ]; then
    fail "Several releases for one tag" "There are $n releases or drafts named $TAG. Delete the extra drafts on the Releases page, keep the one whose target is $short, and run again."
elif [ "$n" = 1 ]; then
    set -- $rels
    id=$1 isdraft=$2 target=$3
    [ "$isdraft" = true ] || fail "Release already published" "Release $TAG is already published; a published release is never changed. Use a new version (e.g. a patch release)."
    [ "$target" = "$SHA" ] || fail "Draft targets another commit" "The draft release $TAG targets $target, but this run builds $short. Dispatch from the draft's commit, or delete the draft (its approval is void) and run again."
    if [ -n "$ASSETS" ]; then
        names=$(gh api "repos/$REPO/releases/$id/assets?per_page=100" -q '.[].name')
        for have in $names; do
            for want in $ASSETS; do
                # shellcheck disable=SC2254
                case "$have" in $want)
                    [ "$REPLACE" = yes ] || fail "Draft already has this asset" "The draft $TAG already has $have from an earlier run of $short. To rebuild it, run again with replace-assets: true (then update the release manifest and get it approved again), or delete the asset first."
                    echo "the draft's $have will be replaced (replace-assets)"
                ;; esac
            done
        done
    fi
    draft=yes
    echo "draft release $TAG exists and targets $short"
fi
echo "draft=$draft"
if [ -n "${GITHUB_OUTPUT:-}" ]; then echo "draft=$draft" >> "$GITHUB_OUTPUT"; fi
