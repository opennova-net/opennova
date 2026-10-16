# TODO

Everything here is work that is **not** a parity divergence: code hardening and project
health. Divergences from the original engine belong in
[docs/divergence-ledger.md](docs/divergence-ledger.md) instead, and
[docs/current-state.md](docs/current-state.md) explains which is which.

## Cleanup & verification backlog

- [ ] Apply the launcher and expansion-distribution retirement (the config left the repo
      with `infra/github`, `on-deploy github` and the downloads/`launcher_ci`/`nw` blocks;
      the PR body has the commands): drop `downloads_bucket_name` and `create_ci_user` from
      the local `infra/aws/terraform.tfvars`; empty `downloads.opennova.net` (every version
      and delete marker); `./deploy/run.sh infra plan` then `apply` (it destroys the bucket
      set, CloudFront, the ACM cert and its validation record, the `downloads` CNAME, the
      `nw` record and `launcher_ci`, and turns the account-wide S3 public access block on);
      delete the five Actions secrets on revx02/onjo01/ondx01 (the repos stay); then drop the
      `us_east_1` provider alias in `infra/aws/main.tf`.

## Project health follow-ups

- [ ] Engine homes for two per-frame rules the bindings still compose (ADR 0042's boundary rule; post-merge audit C-310, C-316, C-314). The render-slot frame loop in `godot/src/env/slot_shadow.cpp` (the capture-with child walk, the stored slot direction, the first-person drape gate, cull before fade, the attached-light / sun ambient branch, the grazing lod) becomes one `renderer::slot_frame` returning each assignment's capture request and drape terms, the binding keeping the RD captures, the eye back-off fold and the uniforms. The particle quad inputs in `ParticleRenderer`'s `build_render_snapshot` (the colour/alpha LUT reads, the scale LUT lerp / 128 with the D-PTL-11 clamp, the tint pick, the flipbook clock, the TOPALIGN roll) move beside `renderer::ParticleFrameCompiler`, the two uncited LUT reads cited; then `particle_renderer.cpp` (2395 lines, ratchet 2500) lifts its atlas/catalog device and material cache out of `Impl`. Same pixels; each lands with a GDExtension build and the GUT suite.
- [ ] Formatting enforcement: `scripts/lint/format_lint.py` checks the lines a change adds (clang-format, ruff, gdlint/gdformat; CI's `format-lint` job, advisory). Remaining: the reformat commits (C++ one per group, whitespace-only, checked by rebuilding and comparing objects; Python and GDScript per tree) plus `.git-blame-ignore-revs`, at a moment the maintainer picks since they conflict with open branches; then `format-lint` runs with `--enforce` as a required check.
