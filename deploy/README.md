# OpenNova deploy toolbox

This directory holds everything needed to deploy a NovaWorld instance. The full
guide is [DEPLOY.md](../DEPLOY.md) at the repo root.

- `run.sh` / `run.ps1` — wrappers that run the toolbox container.
- `bin/on-deploy` — the orchestrator: `secrets check`, `infra plan|apply|destroy|output|list`,
  `github import|plan|apply` (pending retirement, see `TODO.md` and `infra/github/README.md`),
  `aws <args...>`, `app deploy|status|logs|restart|down|sql`, `backup now|list`,
  `state push|pull`.
- `Dockerfile`, `docker-compose.deploy.yml` — the toolbox image.
- `compose/` — the runtime stack: base + dev + prod overlays.
- `env/` — `*.env.tpl` carry `op://` references; `app.prod.env` carries the
  committed non-secret prod values.
- `backup/` — the nightly sqlite backup sidecar.
- `game/` — the web build's image (ADR 0049): the wasm GDExtension, the Godot
  Web export and the nginx the portal routes `game.<domain>` to.

The only secret an operator handles is `OP_SERVICE_ACCOUNT_TOKEN`. The target
host needs only docker.
