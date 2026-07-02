# GitHub provisioning stack (`infra/github`)

Manages the GitHub side of the deployment: the expansion content repositories
(`revx02`, `onjo01`, `ondx01`) and the GitHub Actions secrets their build
workflows need. It complements `infra/aws` so the entire stack — cloud
infrastructure **and** the CI repos that feed the expansion-publish pipeline —
boots from our own infra.

**`expansions.tf` (`local.expansions`) is the single source of truth for
expansions.** Each entry there drives, in one `terraform apply`: the GitHub repo,
its Actions secrets, and the server's DB catalogue seed
(`backend/seed/0002_expansions.generated.sql`, rendered by
`local_file.expansions_seed`). That seed carries each expansion's `github_repo`,
so the server reads the slug→repo mapping from the DB instead of a hardcoded map.
Adding an expansion needs no C++, SQL, or web edits — see "Add an expansion" below.

Run it through the deploy toolbox, not bare terraform, so state lives in the
shared 1Password document and secrets come from the vault:

```bash
./deploy/run.sh github import      # one time — adopt the existing repos
./deploy/run.sh github plan
./deploy/run.sh github apply
```

(`deploy/bin/on-deploy` runs `terraform -chdir=infra/github` with its own
`tfstate-github` 1Password document and injects `TF_VAR_github_token` /
`TF_VAR_github_owner` / `shared_repository_secrets` from `deploy/env/github.env.tpl`.)

## One-time import of the existing repos

The expansion repos already exist on GitHub, so terraform must adopt them rather
than try to create them. `github import` runs (idempotently — it skips repos
already in state):

```bash
terraform import 'github_repository.managed["revx02"]' revx02
terraform import 'github_repository.managed["onjo01"]' onjo01
terraform import 'github_repository.managed["ondx01"]' ondx01
```

The import ID is the short repo name; the owner comes from `var.github_owner`.
After import, `github plan` should show the repos as no-ops (or benign metadata
diffs that `ignore_changes` suppresses), never as creates/destroys. Actions
secrets are write-only and need no import — `apply` sets them.

On a fresh GitHub org with no repos, skip the import and run `apply` directly;
`auto_init = true` gives each new repo a default branch (the release-tag step
needs one).

## The secrets and the publish-callback contract

`shared_repository_secrets` (see `terraform.tfvars.example`) is one set of
secrets applied to **every** expansion repo:

- `AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY` — the `launcher_ci` keys output
  by `infra/aws` (reused; they already grant `PutObject` on the downloads bucket).
- `EXPANSION_PUBLISH_TOKEN` — the bearer the repo's workflow presents to the
  server. Same value as `app-prod/expansion_publish_token`.
- `NOVAWORLD_SERVER_URL` — the publish callback base, e.g. `https://nw.<domain>`.
- `DOWNLOADS_BUCKET` — the S3 bucket the package uploads to.

The pipeline, end to end:

1. An admin POSTs `/api/admin/expansions/<slug>/release` on the server, which
   pushes `refs/tags/<repo_ref>` onto the expansion repo (using
   `EXPANSION_GITHUB_TOKEN`) and records a `tagged` release row.
2. The tag push triggers the expansion repo's build workflow (lives in that
   repo — see `docs/net/expansion-publish-workflow.yml.example` for a template).
   It packages the content, computes `sha256` + size, and uploads to
   `s3://$DOWNLOADS_BUCKET/expansions/<slug>/<repo_ref>.zip`.
3. The workflow calls back:
   `POST $NOVAWORLD_SERVER_URL/admin/internal/expansions/<slug>/publish`
   with `Authorization: Bearer $EXPANSION_PUBLISH_TOKEN` and JSON
   `{version, download_url, sha256, size_bytes, workflow_url, target_commit}`
   (or `.../fail` with `{version, error}` on failure).
4. The server upserts the expansion's file row and flips the release to
   `published`; the package then appears in the web `/expansions` catalogue.

## Add an expansion

1. Add an entry to `local.expansions` in `expansions.tf` (slug = map key) with its
   `github_repo`, `game_slug` (must match a row seeded by
   `backend/seed/0001_games.sql`), `display_name`, `summary`, `version`,
   `package_type`, `install_subdir`, `featured`, `description`, `topics`.
2. `./deploy/run.sh github apply` — creates the repo, sets its secrets, and
   regenerates `backend/seed/0002_expansions.generated.sql` (via
   `local_file.expansions_seed`). If the repo already exists, `github import`
   first.
3. Commit the regenerated seed. CI bakes it into the server image; the server
   UPSERTs the catalogue row on its next boot (preserving any released `version`).

No C++, hand-written SQL, or web edits are needed — the server reads the
catalogue (and the slug→repo mapping) from the DB, and the web reads it from
`/api/expansions`.

## Notes

- `prevent_destroy` is set on every repo to avoid accidental deletion. Remove it
  deliberately if you ever need terraform to destroy one.
- `description`, `homepage_url`, and `topics` are ignored for drift so
  maintainers can adjust them by hand.
- `backend/seed/0002_expansions.generated.sql` is generated — never hand-edit it;
  change `local.expansions` and re-apply.
