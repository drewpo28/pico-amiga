---
description: Publish the draft GitHub release after safety checks (version, clean tree, assets)
---

Publish the draft release of the current version. The version is
`PORT_VERSION` in CMakeLists.txt - the single source of truth.

PORT_VERSION (working tree): !`grep -oP 'set\(PORT_VERSION "\K[0-9.]+' CMakeLists.txt`
PORT_VERSION at HEAD: !`git show HEAD:CMakeLists.txt | grep -oP 'set\(PORT_VERSION "\K[0-9.]+'`
Working tree: !`git status --porcelain`
HEAD vs origin: !`git fetch origin --quiet; git rev-parse HEAD; git branch --show-current; git merge-base --is-ancestor HEAD origin/$(git branch --show-current) && echo "HEAD is pushed" || echo "HEAD is NOT pushed"`
Draft: !`VER=$(grep -oP 'set\(PORT_VERSION "\K[0-9.]+' CMakeLists.txt); gh release view "v$VER" -R drewpo28/pico-amiga --json name,isDraft,isPrerelease,targetCommitish,assets --template '{{.name}} draft={{.isDraft}} prerelease={{.isPrerelease}} target={{.targetCommitish}} assets={{len .assets}}: {{range .assets}}{{.name}} {{end}}' 2>&1 || true`
Repository visibility: !`gh repo view drewpo28/pico-amiga --json visibility --jq .visibility 2>&1`

Every check must pass before asking to publish; on a failure STOP and say what
is wrong and how to fix it (do not fix, commit or push anything yourself):

1. **Version**: the working tree's PORT_VERSION equals HEAD's (an uncommitted
   bump would put the wrong version in the tag's source archive).
2. **Clean tree**: `git status --porcelain` is empty.
3. **HEAD pushed**: HEAD is an ancestor of the remote branch.
4. **Draft**: `v<VERSION>` exists, is a draft (or a pre-release being promoted
   to a full release - say so), and every asset ends in `-amiga-<VERSION>.uf2`.
   Expect **5**: m1p2, m2p2, PCp2, DVp2, z0p2 (build_all.sh's boards). Another
   count -> mention it in the confirmation.
5. **Confirm**: AskUserQuestion with a summary (version, asset count, target
   commit - and whether it differs from HEAD, the repository's visibility) and
   options "Publish v<VERSION>" / "Cancel". Nothing without an explicit yes.
6. **Publish** (after the yes):
   - target differs from HEAD: `gh release edit v<VERSION> -R drewpo28/pico-amiga --target <HEAD sha>`
   - a draft: `gh release edit v<VERSION> -R drewpo28/pico-amiga --draft=false`
   - a pre-release made full: `gh release edit v<VERSION> -R drewpo28/pico-amiga --prerelease=false --latest`
   - verify with `gh release view v<VERSION> -R drewpo28/pico-amiga` and that
     the tag is on the remote.
7. **Report** the release URL; the tag and the "Source code" archives were made
   by GitHub at publish time.
