# OpenNova deploy toolbox

This directory holds everything needed to deploy a NovaWorld instance. The full
guide is [DEPLOY.md](../DEPLOY.md) at the repo root.

- `run.sh` / `run.ps1` — wrappers that run the toolbox container.
- `bin/on-deploy` — the orchestrator (secrets / infra / app / backup / state).
- `Dockerfile`, `docker-compose.deploy.yml` — the toolbox image.
- `compose/` — the runtime stack: base + dev + prod overlays.
- `env/` — `*.env.tpl` carry `op://` references; `app.dev.env` / `app.prod.env`
  carry committed non-secret values.
- `backup/` — the nightly sqlite backup sidecar.

The only secret an operator handles is `OP_SERVICE_ACCOUNT_TOKEN`. The target
host needs only docker.
