---
description: Generate Change History and publish a GitHub pre-release via draft-release.sh --prerelease
argument-hint: [new-version e.g. 0.2.0]
---

Create a GitHub **pre-release** with auto-generated notes. Unlike
/draft-release it is visible at once (marked "Pre-release"; while the
repository is private, to its collaborators only): the tag and the "Source
code" archives are made right away, and an unpushed HEAD is pushed by the
script.

Last PUBLISHED release tag (changelog baseline): !`B=$(gh release list -R drewpo28/pico-amiga --json tagName,isDraft,isPrerelease --jq 'map(select(.isDraft==false and .isPrerelease==false)) | .[0].tagName' 2>/dev/null); [ -n "$B" ] || B=$(git tag --sort=-v:refname | grep '^v' | head -1); [ -n "$B" ] || B="(none - first release: describe the whole project)"; echo $B`
Current PORT_VERSION: !`grep -oP 'set\(PORT_VERSION "\K[0-9.]+' CMakeLists.txt`
Release for this version: !`VER=$(grep -oP 'set\(PORT_VERSION "\K[0-9.]+' CMakeLists.txt); gh release view "v$VER" -R drewpo28/pico-amiga --json name,isDraft,isPrerelease,targetCommitish,assets --template '{{.name}} draft={{.isDraft}} prerelease={{.isPrerelease}} target={{.targetCommitish}} assets={{len .assets}}' 2>&1 || true`
Repository visibility: !`gh repo view drewpo28/pico-amiga --json visibility --jq .visibility 2>&1`
Working tree: !`git status --porcelain | head -20`
HEAD vs origin: !`git fetch origin --quiet; git rev-parse --short HEAD; git branch --show-current; git merge-base --is-ancestor HEAD origin/$(git branch --show-current) && echo "HEAD is pushed" || echo "HEAD is NOT pushed"`
Commits since the PUBLISHED baseline: !`B=$(gh release list -R drewpo28/pico-amiga --json tagName,isDraft,isPrerelease --jq 'map(select(.isDraft==false and .isPrerelease==false)) | .[0].tagName' 2>/dev/null); [ -n "$B" ] || B=$(git tag --sort=-v:refname | grep '^v' | head -1); if [ -n "$B" ]; then git log --oneline $B..HEAD; else git log --oneline | wc -l; echo "commits in all (no release yet)"; fi`

Steps:

0. **Check the state above** - never assume it from an earlier run:
   - `release not found` -> the script creates the pre-release;
   - `draft=true` -> the script replaces its files and notes and PROMOTES it;
   - `draft=false prerelease=true` -> the script DELETES and RECREATES it (tag
     included) at HEAD, so tag, source archives and date match the new build;
   - `draft=false prerelease=false` -> STOP: published as a full release; the
     user must bump PORT_VERSION (or pass a new version) first.
   - Uncommitted changes in the working tree -> STOP and list them: the
     firmware is built from the tree but the tag points at HEAD.

1. **Build** every board: `./build_all.sh` (background, ~2 minutes). It lists
   the built-in ROMs left out of the images, if any - report them.

2. **Check**: `./check-release.sh` - each image's heap and the host tests
   (tmds_test, smoke). A FAIL -> stop and report. A WARN -> mention it in the
   confirmation.

3. **Notes** for `<last PUBLISHED release tag>..HEAD`, written to
   `build-all/logs/release-notes-draft.md` as raw markdown (no code fence; it
   goes into the release body after the Downloads table the script builds):
   - first release (no baseline): `## What it is` and a short feature overview
     from README.md (machines, boards, video and audio outputs, disks, menu),
     not a commit list;
   - later: `## Change History` + `<br>`, then the entries by the rules of
     `.claude/commands/getch.md` (grouping, skip-list, format
     `- **Added/Fixed/Improved <Name>** - ...`). No meaningful commits -> stop.

4. **Confirm**: AskUserQuestion with a summary - version, the boards built,
   the check's result, whether a draft is promoted or a pre-release recreated,
   whether HEAD will be pushed, the repository's visibility - and options
   "Publish pre-release v<VERSION>" / "Cancel". Nothing is published without
   an explicit yes.

5. **Publish** (background, wait for it):
   - no version argument: `./draft-release.sh --skip-build --prerelease --notes build-all/logs/release-notes-draft.md`
     (step 1's images);
   - with one: `./draft-release.sh $1 --prerelease --notes build-all/logs/release-notes-draft.md`
     (bumps, rebuilds), then `./check-release.sh` once more and report it.
   - If the script refuses (a dirty tree with a bump), report that; do not
     commit anything yourself.

6. **Report**: the notes, the pre-release URL, and that making it a full
   release later is `gh release edit vX.Y.Z -R drewpo28/pico-amiga --prerelease=false --latest`.
