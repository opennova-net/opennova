# OpenNova deploy toolbox

This directory holds everything needed to deploy a NovaWorld stack to AWS. The full
guide is [DEPLOY.md](../DEPLOY.md) at the repo root.

- `run.sh` / `run.ps1`: wrappers that build (when it changed) and run the toolbox
  container. `--deployment NAME` (or `OPENNOVA_DEPLOYMENT`) picks the deployment: `prod`
  by default, or another such as `staging`.
- `bin/on-deploy`: the orchestrator (`./deploy/run.sh help` prints its commands):
  - `secrets init|check|sync [--prune]`: the vault's generated secrets and Stripe
    placeholders, the checks (Stripe's forms and modes among them), the copy to SSM;
  - `stripe check|setup [--mode test|live]`: the support tiers' Stripe objects (products
    and monthly prices, the Customer Portal configuration, the webhook endpoint) read, or
    made where missing, their ids and the webhook's signing secret into the vault item;
  - `infra plan|apply|destroy|state-rm|import|output|list`: Terraform in the
    deployment's workspace, its state a 1Password document; `apply` applies the plan it
    saved, refuses one that destroys or replaces an Elastic IP or its association, the
    service instance or its interface, the database or a bucket unless
    `--allow-destroy`, and on prod asks for `yes` (or `--yes`) first;
  - `github plan|apply|import|output|list`: the GitHub organization (`infra/github`;
    DEPLOY.md, "GitHub"): its settings, members, CI runners, repositories, branch
    protection and Actions secrets, one stack with no deployments, its state the vault
    document `tfstate-github-org`; `apply` asks for `yes` (or `--yes`) first;
  - `app deploy|releases|rollback|converge|status|restart|down|logs|shell`: releases
    (bundles in S3, the images pinned by digest) converged on the host through SSM,
    CloudWatch logs, a Session Manager shell;
  - `db bootstrap|psql|reset-db|snapshot`, `backup now|list|restore`: the RDS database
    and its pg_dump copies;
  - `fleet status|drain|end|roll`: the hosted game servers' machines;
  - `hosted data-snapshot --data-set <slug>`: a data set's game data into a snapshot;
  - `state push|pull [--force]`, `aws <args...>`.
- `Dockerfile`, `docker-compose.deploy.yml`: the toolbox image (Debian: the docker CLI
  with compose and buildx, op, Terraform, the AWS CLI v2, the Session Manager plugin;
  each a pinned, checksummed release).
- `host/`: what runs on the service instance. `service-user-data.sh` is its first boot
  (Docker, the AWS CLI, the metadata fence, the converge unit); `converge.sh` brings it
  to the deployment's release (Terraform puts it in the SSM converge document; the
  instance runs it at every boot, `app deploy` sends it) and installs the helpers the
  other commands run there (`opennova-compose`, `opennova-psql`, `opennova-admin-api`).
- `compose/`: the runtime stack: the base file, the dev overlay (local work) and the prod
  overlay, which the instance runs from each release bundle.
- `env/`: `app.prod.env`, the committed non-secret values every release carries;
  `*.tpl`, the `op://` references (`runtime.env.tpl`: the service's secrets, which
  `secrets sync` copies to SSM; `terraform.env.tpl`: Terraform's; `aws.env.tpl`: the
  toolbox's own AWS calls).
- `backup/`: the pg_dump sidecar (nightly dumps to S3, `backup now|list|restore`).
- `hosted-data/`: the helper image `hosted data-snapshot` builds on the operator's docker
  to write one data set's game data into a private encrypted EBS snapshot.

The only secret an operator handles is `OP_SERVICE_ACCOUNT_TOKEN`. Nothing reaches the
service instance over SSH: it has no key and no port 22.

## Resetting the service database

The database is durable: migrations are append-only, and `db reset-db` refuses while a
subscription grants hosting:

```bash
./deploy/run.sh db reset-db --yes-drop-database
```

It dumps the database to S3 first (`--no-backup` skips that), drops and recreates it
empty, and starts the server on it, whose first boot applies every migration.
`--orphan-subscriptions` resets past the billing guard. Details:
[DEPLOY.md](../DEPLOY.md), "Resetting the service database".
