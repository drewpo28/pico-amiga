---
description: Generate Change History and create a draft GitHub release via draft-release.sh
argument-hint: [new-version e.g. 0.2.0]
---

Create a draft GitHub release with auto-generated Change History notes.

Last PUBLISHED release tag (changelog baseline): !`B=$(gh release list -R drewpo28/pico-amiga --json tagName,isDraft,isPrerelease --jq 'map(select(.isDraft==false and .isPrerelease==false)) | .[0].tagName' 2>/dev/null); V=$(grep -oP 'set\(PORT_VERSION "\K[0-9.]+' CMakeLists.txt); [ -n "$B" ] || B=$(git tag --sort=-v:refname | grep '^v' | grep -vx "v$V" | head -1); [ -n "$B" ] || B="(none - first release: describe the whole project)"; echo $B`
Current PORT_VERSION: !`grep -oP 'set\(PORT_VERSION "\K[0-9.]+' CMakeLists.txt`
Release for this version: !`VER=$(grep -oP 'set\(PORT_VERSION "\K[0-9.]+' CMakeLists.txt); gh release view "v$VER" -R drewpo28/pico-amiga --json name,isDraft,isPrerelease,targetCommitish,assets --template '{{.name}} draft={{.isDraft}} prerelease={{.isPrerelease}} target={{.targetCommitish}} assets={{len .assets}}' 2>&1 || true`
Commits since the PUBLISHED baseline: !`B=$(gh release list -R drewpo28/pico-amiga --json tagName,isDraft,isPrerelease --jq 'map(select(.isDraft==false and .isPrerelease==false)) | .[0].tagName' 2>/dev/null); V=$(grep -oP 'set\(PORT_VERSION "\K[0-9.]+' CMakeLists.txt); [ -n "$B" ] || B=$(git tag --sort=-v:refname | grep '^v' | grep -vx "v$V" | head -1); if [ -n "$B" ]; then git log --oneline $B..HEAD; else git log --oneline | wc -l; echo "commits in all (no release yet)"; fi`

Steps:

0. **Check the "Release for this version" line above** - never assume state
   from an earlier run:
   - `release not found` -> the script creates a fresh draft;
   - existing with `draft=true` -> the script replaces its files and notes;
   - existing with `draft=false` -> STOP: this version is published; the user
     must bump PORT_VERSION (or pass a new version argument) first.

1. **Generate the Change History** for `<last PUBLISHED release tag>..HEAD`
   (NOT the newest tag: a version that exists only as a pre-release has not
   been released, so its notes describe the whole version). With no release yet
   (the first one), write a short **feature overview** instead of a commit
   list: what the emulator runs, the boards, video and audio outputs, disks,
   the menu - from README.md, not from the commit log. Otherwise follow
   `.claude/commands/getch.md` exactly (grouping, skip-list, entry format
   `- **Added/Fixed/Improved <Name>** - ...`). No meaningful commits -> stop
   and say there is nothing to release.

2. **Write the notes** to `build-all/logs/release-notes-draft.md` - raw markdown
   WITHOUT a code fence (it goes verbatim into the release body, after the
   Downloads table the script builds). Start with this exact header:

   ```
   ## Change History
   <br>

   ```

   (for the first release: `## What it is` instead).

3. **Run the release script**:
   - With a version argument: `./draft-release.sh $1 --notes build-all/logs/release-notes-draft.md`
     (bumps PORT_VERSION and commits it; no push).
   - Without: `./draft-release.sh --notes build-all/logs/release-notes-draft.md`.
   - The build of every board takes minutes: run it in the background and wait.
   - A dirty working tree with a version bump requested: the script refuses -
     report that, do not commit anything yourself.

4. **Report**: the notes, the draft's URL, the built-in ROMs the build left out
   (build_all.sh lists them), and that publishing is
   `gh release edit vX.Y.Z -R drewpo28/pico-amiga --draft=false` (or `/release`).
