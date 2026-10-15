# OpenNova deploy toolbox

This directory holds everything needed to deploy a NovaWorld instance. The full
guide is [DEPLOY.md](../DEPLOY.md) at the repo root.

- `run.sh` / `run.ps1` — wrappers that run the toolbox container.
- `bin/on-deploy` — the orchestrator: `secrets check`, `infra plan|apply|destroy|output|list`,
  `aws <args...>`, `app deploy|status|logs|restart|down|sql|reset-db`, `backup now|list`,
  `state push|pull [--force]`, `hosted status|data-snapshot` (DEPLOY.md, "Hosted game servers").
- `Dockerfile`, `docker-compose.deploy.yml` — the toolbox image.
- `compose/` — the runtime stack: base + dev + prod overlays.
- `env/` — `*.tpl` carry `op://` references; `app.prod.env` carries the
  committed non-secret prod values.
- `backup/` — the nightly `pg_dump` backup sidecar (the service database to S3).
- `hosted-data/`: the helper image `hosted data-snapshot --data-set <slug>` builds on
  the operator's docker to write one data set of the hosted game servers' data, once,
  into a private encrypted EBS snapshot (one snapshot per data set).
- `game/` — the web build's image (ADR 0049): the wasm GDExtension, the Godot
  Web export and the nginx the portal routes `game.<domain>` to.

The only secret an operator handles is `OP_SERVICE_ACCOUNT_TOKEN`. The target
host needs only docker.

## Resetting the service database

A deploy whose PR notes say "the service DB must be reset" carries a migration
edited in place, which never runs again on an existing database. Deploy it with:

```bash
./deploy/run.sh app reset-db --yes-drop-database --tag sha-abc1234
```

It takes a final backup (skip with `--no-backup`), deletes the SQLite files from
the `nw_data` volume and deploys that tag onto the empty database. Earlier backups
stay in S3; undoing a reset means restoring one AND deploying the server tag that ran
before it (the plan prints it), since a newer server skips every migration a restored
backup records. Details: [DEPLOY.md](../DEPLOY.md), "Resetting the service database".
