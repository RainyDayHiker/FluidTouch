# 09 — Git workflow

Goals: keep taking platform fixes from upstream FluidTouch for a long time (G11), keep the
new work reviewable, and always be able to say which commit is on the device.

## 1. Remotes

```powershell
git remote add upstream https://github.com/jeyeager65/FluidTouch.git
git fetch upstream
```

`origin` is Jeffrey's fork. Never push to `upstream`.

## 2. Branches

| Branch | Purpose | Rules |
|--------|---------|-------|
| `main` | Mirror of upstream's default branch | Fast-forward only from `upstream`. No LowRider Touch commits |
| `lrt/main` | The new firmware; always builds (firmware and simulator) and boots | Changes arrive by merging topic branches. Never rebased or force-pushed |
| `lrt/<milestone>-<topic>` | One piece of work, e.g. `lrt/m1-client-events`, `lrt/m2-setup-screen` | Short-lived, branched from `lrt/main`, deleted after merge. May be rebased before merging |
| `fix/<topic>` | A fix to upstream-owned code that could be offered to upstream | Branched from `main`, contains only that fix, then merged into `lrt/main`. Keep it local unless a pull request to upstream is being opened: `fix/**` matches upstream's `build-branch-preview.yml`, so pushing it to the fork runs that workflow there (a preview build committed to the fork's `gh-pages`) if Actions are enabled. Delete it once merged, or once the upstream pull request is closed |

First setup:

```powershell
git checkout main
git merge --ff-only upstream/main
git checkout -b lrt/main
git add CLAUDE.md specs
git commit -m "specs: add LowRider Touch M0 specs and handoff"
git push -u origin lrt/main
```

(`specs/` and `CLAUDE.md` are untracked files, so they follow the checkout.)

## 3. Commits

- One logical change per commit; it builds for the firmware and the simulator.
- Subject: `<area>: <what changed>`, imperative, under 72 characters. Areas:

| Area | Covers |
|------|--------|
| `platform` | Any edit to upstream-owned files: `src/core`, `src/network`, `src/ui`, `src/main.cpp`, `platformio.ini`, `include/` files that upstream owns |
| `sim` | `sim/`, including the fake FluidNC |
| `app` | `src/app` |
| `services` | `src/services` |
| `gcode` | `lib/gcode` and its tests and golden data |
| `specs` | `specs/`, `CLAUDE.md` |
| `build` | CI, scripts, partition tables |

- **Never mix `platform` or `sim` changes with new code in one commit.** This is the rule
  that keeps upstream merges manageable: a conflict then always lands in a small commit
  whose whole purpose is visible, and such a commit can be cherry-picked to an
  `fix/<topic>` branch if it turns out to be useful upstream.
- In a `platform` commit body, say why the edit is needed and which spec section lists it
  ([02 §2](02-architecture.md#2-changes-to-kept-code)). If an edit is not listed there, add
  it to the list in the same commit.
- A decision change updates `specs/` in the same commit as the code.
- Not committed: `platformio_local.ini`, `sim/data/`, `sim/build/`, `.pio/`, anything with
  WiFi credentials. (All already gitignored; check before adding new local files.)

## 4. Merging work into `lrt/main`

- Open a pull request from the topic branch to `lrt/main` **in the fork** (set the base
  repository to `RainyDayHiker/FluidTouch`; GitHub defaults to the upstream repo).
- Merge with a merge commit, not squash, so `platform` commits stay separate.
- Before merging: firmware builds, simulator builds, the library test runner passes (from
  M4), and the classic UI still starts in the simulator.

## 5. Taking upstream changes

At milestone boundaries, or when upstream fixes something needed:

```powershell
git fetch upstream
git checkout main && git merge --ff-only upstream/main && git push origin main
git checkout lrt/main && git merge main
```

Merge, don't rebase: `lrt/main` is long-lived and published. Resolve conflicts in favour of
upstream's structure, re-applying the small `platform` edits on top. After the merge,
rebuild both targets and check the classic UI and the new UI in the simulator. If upstream
changed `FluidNCClient`'s status parsing or the upload code, re-check the services that use
them.

## 6. Tags and versions

- Tag every build that goes onto the machine for real use: `lrt-m1`, `lrt-m2`,
  `lrt-m2.1`... The About page shows the tag and short commit hash.
- Leave upstream's `VERSION` / `FLUIDTOUCH_VERSION` alone (it changes with upstream merges);
  add a separate `LRT_VERSION`.

## 7. CI

The repo has upstream's workflows (`build-firmware.yml`, `build-branch-preview.yml`). Before
the first push of `lrt/main`, read what they trigger on and what they publish (releases, the
web installer on GitHub Pages) and restrict or disable anything that would publish from
`lrt/*` branches. Add a workflow for `lrt/**` that builds the Advance v1.3 firmware and the
simulator and, from M4, runs the `lib/gcode` tests. Each such change is a `build:` commit.

## 8. CNC-Tools

Work there (the golden export script, the Move operation spec and implementation) follows
that repo's own conventions. Golden data vendored into `lib/gcode/test/golden/` records the
CNC-Tools commit hash it was exported from; regenerate rather than hand-edit.
