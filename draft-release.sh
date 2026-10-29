#!/bin/bash
# Create (or update) a GitHub release with every board's .uf2 (after
# pico-speccy's draft-release.sh).
#
# By default the release is a DRAFT: the git tag and the "Source code" archives
# appear only when it is published (gh release edit vX.Y.Z --draft=false). With
# --prerelease it is published at once as a pre-release (tag and archives made
# now); an existing draft of the version is promoted, an already published
# pre-release is deleted and recreated, tag included, so the tag, the archives
# and the date match this build. A published full release is never touched.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

REPO="drewpo28/pico-amiga"
SKIP_BUILD=false
PRERELEASE=false
NOTES_FILE=""
NEW_VERSION=""

usage() {
    cat <<EOF
Usage: $0 [NEW_VERSION] [--skip-build] [--prerelease] [--notes FILE]

  NEW_VERSION   e.g. 0.2.0: bumps PORT_VERSION in CMakeLists.txt and commits it.
                Without it the current PORT_VERSION is released.
  --skip-build  Reuse firmware/*.uf2 from a previous build_all.sh run.
  --prerelease  Publish at once as a pre-release instead of a draft (an
                unpushed HEAD - e.g. the version bump - is pushed first).
  --notes FILE  Release notes (markdown) after the Downloads table. Default:
                GitHub's generated notes.
EOF
    exit 0
}

while [ $# -gt 0 ]; do
    case "$1" in
        --skip-build) SKIP_BUILD=true; shift ;;
        --prerelease) PRERELEASE=true; shift ;;
        --notes)      NOTES_FILE="$2"; shift 2 ;;
        -h|--help)    usage ;;
        [0-9]*.[0-9]*.[0-9]*) NEW_VERSION="$1"; shift ;;
        *) echo "Unknown argument: $1 (see --help)"; exit 1 ;;
    esac
done

CUR_VERSION=$(grep -oP 'set\(PORT_VERSION "\K[0-9.]+' CMakeLists.txt)
[ -n "$CUR_VERSION" ] || { echo "Error: no PORT_VERSION in CMakeLists.txt"; exit 1; }

if [ -n "$NEW_VERSION" ] && [ "$NEW_VERSION" != "$CUR_VERSION" ]; then
    if ! git diff --quiet || ! git diff --cached --quiet; then
        echo "Error: the working tree has uncommitted changes; the version bump is a commit of its own."
        exit 1
    fi
    echo "Bumping PORT_VERSION: $CUR_VERSION -> $NEW_VERSION"
    sed -i "s/set(PORT_VERSION \"$CUR_VERSION\")/set(PORT_VERSION \"$NEW_VERSION\")/" CMakeLists.txt
    git add CMakeLists.txt
    git commit -m "Version $NEW_VERSION"
    VER="$NEW_VERSION"
else
    VER="$CUR_VERSION"
fi
TAG="v$VER"

# The release's commit must be on GitHub: HEAD when pushed; a pre-release pushes
# it (its tag is made now); a draft targets the remote tip and is retargeted at
# publish time (/release checks that).
git fetch origin --quiet
BRANCH=$(git branch --show-current)
HEAD_SHA=$(git rev-parse HEAD)
if git merge-base --is-ancestor "$HEAD_SHA" "origin/$BRANCH" 2>/dev/null; then
    TARGET_SHA="$HEAD_SHA"
elif $PRERELEASE; then
    echo "Pre-release: pushing $BRANCH so the tag can point at HEAD"
    git push origin "$BRANCH"
    TARGET_SHA="$HEAD_SHA"
else
    TARGET_SHA=$(git rev-parse "origin/$BRANCH")
    echo "Note: HEAD is not pushed - the draft targets origin/$BRANCH ($TARGET_SHA)."
fi

if $SKIP_BUILD; then
    echo "Skipping the build (reusing firmware/)"
else
    ./build_all.sh
fi

FILES=(firmware/*-amiga-"$VER".uf2)
[ -f "${FILES[0]}" ] || { echo "Error: no firmware/*-amiga-$VER.uf2 - run without --skip-build"; exit 1; }
STALE=$(ls firmware/*.uf2 | grep -v -- "-$VER.uf2" || true)
[ -z "$STALE" ] || { echo "Error: firmware/ holds files of another version:"; echo "$STALE"; exit 1; }
echo ""
echo "Firmware (${#FILES[@]}):"
printf '  %s\n' "${FILES[@]##*/}"
echo ""

# The body: the Downloads table of what was built, then the notes.
mkdir -p build-all/logs
BODY_FILE="build-all/logs/release-body.md"
python3 tools/release_downloads.py "$REPO" "$TAG" "${FILES[@]}" > "$BODY_FILE"
if [ -n "$NOTES_FILE" ]; then
    [ -f "$NOTES_FILE" ] || { echo "Error: notes file not found: $NOTES_FILE"; exit 1; }
    cat "$NOTES_FILE" >> "$BODY_FILE"
    NOTES_ARGS=(--notes-file "$BODY_FILE")
else
    NOTES_ARGS=(--notes-file "$BODY_FILE" --generate-notes)
fi

EXISTS=false
if gh release view "$TAG" -R "$REPO" >/dev/null 2>&1; then
    EXISTS=true
    IS_DRAFT=$(gh release view "$TAG" -R "$REPO" --json isDraft --template '{{.isDraft}}')
    IS_PRE=$(gh release view "$TAG" -R "$REPO" --json isPrerelease --template '{{.isPrerelease}}')
fi

if $EXISTS && [ "$IS_DRAFT" != "true" ]; then
    if ! $PRERELEASE || [ "$IS_PRE" != "true" ]; then
        echo "Error: $TAG is already PUBLISHED - bump PORT_VERSION for a new release."
        exit 1
    fi
    echo "Pre-release $TAG is published - recreating it at $TARGET_SHA"
    gh release delete "$TAG" -R "$REPO" --cleanup-tag -y
    git tag -d "$TAG" >/dev/null 2>&1 || true
    EXISTS=false
fi

if $EXISTS; then
    echo "Draft $TAG exists - replacing its files and notes"
    gh release upload "$TAG" -R "$REPO" --clobber "${FILES[@]}"
    gh release edit "$TAG" -R "$REPO" --notes-file "$BODY_FILE"
    if $PRERELEASE; then
        gh release edit "$TAG" -R "$REPO" --target "$TARGET_SHA" --draft=false --prerelease
    fi
else
    TYPE_ARGS=(--draft)
    $PRERELEASE && TYPE_ARGS=(--prerelease)
    gh release create "$TAG" -R "$REPO" "${TYPE_ARGS[@]}" --title "$VER" \
        --target "$TARGET_SHA" "${NOTES_ARGS[@]}" "${FILES[@]}"
fi
$PRERELEASE && { git fetch origin --tags --force --quiet 2>/dev/null || true; }

echo ""
gh release view "$TAG" -R "$REPO" --json url,isDraft,isPrerelease \
    --template 'Release: {{.url}}  (draft: {{.isDraft}}, prerelease: {{.isPrerelease}})
'
if $PRERELEASE; then
    echo "Make it a full release with:  gh release edit $TAG -R $REPO --prerelease=false --latest"
else
    echo "Publish with:  gh release edit $TAG -R $REPO --draft=false"
fi
