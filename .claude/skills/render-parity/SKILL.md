---
name: render-parity
description: Refreshes this repo's registered retail/OpenNova render-parity screenshots — the T3 headline evidence under screenshots/parity/render-lighting-2026-08/ — by freezing a commit, capturing the fixture catalog through the Godot probe, re-registering the retail bundles, building the side-by-side sheets, and repointing every doc and test that names the publication. Use when asked to refresh, update, recapture, or republish the render parity screenshots or side-by-sides, when a render slice needs its retail comparison re-shot, or when the published evidence names a commit that is no longer on master.
---

# Refresh the registered render-parity publication

`docs/render/render-parity-runbook.md` is the procedure; read it before doing
anything. This skill is the orchestration around it: what to decide, what order
to work in, and what to check before calling it done. The standing rules are
[ADR 0023](../../../docs/adr/0023-render-visual-parity.md) — the by-eye retail
pass is the gate, MAE/RMS are descriptive, tolerances never widen.

## 1. Scope the refresh

Read `docs/render/README.md` for the current publication path, frozen source
commit, and catalog. Then answer three questions before touching anything:

- **Is the published frozen source still on `master`?** `git merge-base
  --is-ancestor <sha> master`. A squash-merged PR SHA is the usual reason a
  refresh is needed at all.
- **Does retail need re-shooting?** Almost never. Retail pixels do not change
  when our source changes, and the retained raw bundles re-register against a
  new commit with no game launch. Only a change to the catalog poses, the video
  profile contract, the capture-mode contract, onHook, or the retail install
  forces a fresh capture — and that needs the onHook MCP server registered and
  the client restarted first, so it cannot start mid-session.
- **Where are the retained bundles?** Search prior worktree `.scratch` trees for
  `registered.json` siblings. A usable bundle has all seven artifacts per
  fixture (runbook section 0). Verify the four provenance binaries still hash to
  the runbook's pinned values before planning around them.

Report the answers before starting; a refresh that silently re-shoots retail
costs hours.

## 2. Land the slice's changes FIRST, then freeze

The OpenNova manifest binds a clean tree plus the exact Godot and GDExtension
binaries. Commit everything — skill, runbook, tooling, any render fix — before
capturing, and do not rebuild afterwards. That commit is `$sourceCommit` for
both legs.

Preflight per `oned-run`: submodules, `bash scripts/build_godot.sh`, one
`--import`, `GODOT_BIN` at the main checkout's non-console
`Godot_v4.6.1-stable_win64.exe`. Build outside the sandbox.

## 3. Smoke one fixture before the sweep

Capture `00tra-courtyard-retail` alone (runbook section 2) and read the probe's
PASS line. The probe fail-closes on the matched-presentation runtime witness —
M16 Burst 30/270, character `0x0402`, bare `IndoArms.3di`, HUD hidden but its
CanvasLayer live, viewmodel and terrain on, catalog pose. Armory, customization
and viewmodel drift on the branch surface here and nowhere earlier.

If the witness fails: fix the seam or mint a ledger row. Never relax the
contract to get a green capture — that is the fake-green rule.

Then check the smoke manifest's `mission.expansion` before sweeping. Both
engines must mount the same install and the same expansion (`revx02`); the
probe's `jox01` default belongs to a different catalog and a different install,
and the witness passes either way, so nothing but that field catches it.

Only then loop the remaining fourteen catalog ids.

## 4. Retail, comparisons, publication

Runbook sections 3a (or 3b), 4. Two things are easy to get wrong:

- Registration reads the LIVE `game.cfg` and `weapon.sav`, so retail must be
  staged while you register and restored the moment you finish. Verify both
  hashes against the runbook table on the way in and on the way out.
- `build_retail_side_by_side.py` fail-closes when the registration and the
  OpenNova manifest name different source commits. That is the intended
  behavior, not a bug to work around.

`scripts/render/publish_registered_comparisons.py` assembles the fixture
directories and generates the publication README's identity and metric tables.
Do not hand-write those tables. Delete the superseded publication directory in
the same commit.

## 5. Repoint and verify

Exactly these name the publication: `tests/test_retail_render_evidence.py`
(`PUBLISHED_EVIDENCE_ROOT`, `CURRENT_SOURCE_COMMIT`), `docs/render/README.md`,
and `docs/render/render-lighting-parity-2026-08-15.md`. Grep for the old
directory name and the old SHA to confirm nothing else does.

    uv run --frozen pytest tests/test_retail_render_evidence.py -v
    uv run --frozen pytest tests/test_retail_capture_registration.py tests/test_render_comparison_tool.py
    ctest --test-dir build -C Release -R "renderer|render"

The evidence suite skips as pass on LFS pointer stubs — confirm the
publication-pinned tests actually ran. The render ctest must pass without
re-dumping any golden; a refresh is not a behavior change.

Touch `docs/divergence-ledger.md` only if a row's visual status genuinely moved,
and regenerate the scoreboard with `python scripts/lint/ledger_check.py --write`
when one does.

## 6. Attest

Look at all fifteen side-by-sides. T3 is a maintainer attestation, not a metric
threshold: write the scene-by-scene read into the PR description, naming what
still diverges (water reflection and noise, fire particles, vegetation and live
actors, night exposure, viewmodel pose and lighting are the known residuals).

Done = the publication directory is complete and hash-consistent, the superseded
one is deleted, every reference is repointed, the tests above pass having
actually run, retail config is restored to its original hash, and the PR
description carries the scene-by-scene attestation.
