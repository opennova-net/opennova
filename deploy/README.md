# OpenNova deploy toolbox

This directory holds everything needed to deploy a NovaWorld stack to AWS. The full
guide is [DEPLOY.md](../DEPLOY.md) at the repo root; its checklist "From zero, in order" is
the whole bring-up.

- `run.sh` / `run.ps1`: wrappers that build (when it changed) and run the toolbox
  container. `--deployment prod|staging` picks the deployment, each in an AWS account of
  its own (DEPLOY.md, "Deployments: dev, staging and prod"). A command that changes a
  deployment must name it (there is no default and no environment variable for it); a
  read-only one reads prod unless told otherwise, and every command prints the
  deployment and the account it acts in. Prod's changes ask for the typed word `prod`
  (or `--yes`).
- `bin/on-deploy`: the orchestrator (`./deploy/run.sh help` prints its commands):
  - `secrets init|check|sync [--prune]`: the vault's generated secrets and placeholders
    (the deployment's items and the shared ones), the checks (Stripe's forms and modes
    among them), the copy to SSM;
  - `stripe check|setup`: the support tiers' Stripe objects in the deployment's mode
    (live for prod, test for staging: products and monthly prices, the Customer Portal
    configuration, the webhook endpoint on the deployment's site) read, or made where
    missing, their ids and the webhook's signing secret into the vault item;
  - `infra plan|apply|destroy|state-rm|import|output|list`: Terraform of the deployment's
    stack (`infra/aws`), its state in the deployment's own account's state bucket; `apply`
    applies the plan it saved, refuses one that destroys or replaces an Elastic IP or its
    association, the load balancer, the cluster or a program's service, the database,
    the cache or a bucket unless `--allow-destroy`;
  - `org plan|apply|import|output|list`: the Organization (`infra/org`), as the management
    account's `opennova-org` user (vault item `aws-org`), no deployment: the member
    accounts (each deployment's and the CI runners'), and in each deployment's account,
    through its `OrganizationAccountAccessRole`, the Terraform state bucket, the
    role boundary, the deploy user `opennova-deploy` and its key, and the GitHub deploy
    role; its state in prod's state bucket; an apply writes each deployment's bucket, key
    and role into its vault item `aws-<deployment>`;
  - `zone plan|apply|import|output|list`: the Cloudflare zone's shared settings
    (`infra/zone`: Full (strict), each deployment's edge authentication rule, the Stripe
    webhook's rule, Email Routing), no deployment, its state in prod's state bucket;
  - `github plan|apply|import|output|list`: the GitHub organization (`infra/github`;
    DEPLOY.md, "GitHub"): its settings, members, repositories, the deploy environments,
    branch protection and Actions secrets, no deployment, its state in prod's state
    bucket;
  - `ci plan|apply|import|output|list`: the CI runners (`infra/ci`; DEPLOY.md, "CI
    runners"): RunsOn in the ci account (vault item `aws-ci`), as `opennova-org` through
    that account's `OrganizationAccountAccessRole`, no deployment, its state in prod's
    state bucket;
  - `app deploy|images|releases|rollback|converge|status|restart|down|up|logs|shell`:
    releases (each program's task definition revision with the images pinned by digest,
    the manifest in SSM and S3; `app deploy --deployment prod --promote staging` takes
    staging's release by digest) rolled out to the five ECS services, CloudWatch logs,
    an ECS Exec shell;
  - `db bootstrap|psql|reset-db|snapshot`, `backup now|list|restore|import`: the RDS
    database and its pg_dump copies, through one-off tasks of the backup task definition
    (`backup import --from-deployment prod KEY` brings prod's dump into staging alone);
  - `fleet status|logs|drain|end|roll|bake-image`: the hosted game servers' machines
    (`fleet logs MACHINE [SLOT|hostd]`: a machine's logs from CloudWatch; their game data
    is the site's Game data page, DEPLOY.md "Hosted game servers", not the toolbox's);
  - `aws <args...>`: the AWS CLI in the deployment's account.

  Every Terraform state lives in an S3 bucket of a deployment's account, versioned and
  encrypted, locked by S3's own lock file: nothing to push or pull.

  It keeps the help, the options, the helpers every command shares (the deployment,
  the vault, AWS) and `main`; the commands live in the files it sources from `bin/lib/`,
  one per responsibility: `secrets.sh`, `stripe.sh`, `infra.sh` (`infra`, `org`, `zone`,
  `github`, `ci`), `app.sh` (the releases on ECS, ECS Exec, the one-off tasks), `db.sh` (`db`,
  `backup`) and `fleet.sh` (`fleet`). The toolbox runs it from the repo, mounted
  at `/workspace`, so the image copies none of it. `bin/tests/deployment_guard_test.sh`
  checks the deployment guard offline, `bin/tests/vault_requests_test.sh` the vault's
  requests (each command's counted) and the values the templates resolve to (CI's infra
  workflow runs both).
- `Dockerfile`, `docker-compose.deploy.yml`: the toolbox image (Debian: the docker CLI
  with compose and buildx, op, Terraform, the AWS CLI v2, the Session Manager plugin, which
  ECS Exec uses; each a pinned, checksummed release).
- `host/fleet-image.sh`: the user data of `fleet bake-image`'s one-off builder, which bakes
  the hosted machines' image.
- `compose/`: the local stack (`scripts/dev_stack.sh`, DEVELOPING.md; dev: no AWS account, no
  credentials of any deployment): the five programs, the portal, PostgreSQL and Valkey (the
  base file and the dev overlay), and the hosted servers' machines as containers (the hosting
  overlay). A deployment runs the same images as ECS services (`infra/aws/programs.tf`), not
  through compose.
- `env/`: the `op://` references (`runtime.env.tpl`: the programs' secrets, which `secrets
  sync` copies to SSM and ECS hands the tasks; `terraform.env.tpl`: a deployment stack's;
  `zone.env.tpl`: the zone stack's; `github.env.tpl`: the organization stack's;
  `org.env.tpl`: the org stack's; `ci.env.tpl`: the CI runners' stack's;
  `aws.env.tpl`: which access key a command acts with, its deployment's deploy user's
  (`aws-<deployment>`) or, for `org` and `ci`, `opennova-org`'s (`aws-org`); `billing.*.env.tpl`: Stripe's test mode for local work and CI,
  through the restricted key alone). No tracked file holds a value: the vault holds
  every identifier a rebuild needs (DEPLOY.md, "Rebuilding from the vault", the vault's
  contract item by item). The toolbox
  resolves the references itself, with no `op run`, `op inject` or `op read`: a run checks
  the token once, lists the vault once and reads each item it needs once, by its id, into
  a cache in the tmpfs that goes when the command ends, and every field and template
  resolves from that cache into a command's environment (Terraform's, the AWS CLI's) or a
  tmpfs file (SSM's), never an argument or an output. The service account's requests are
  rate-limited (DEPLOY.md, "2. Mint the service-account token", says what each command
  costs).
- `backup/`: the backup task's image (nightly dumps to the deployment's own bucket,
  `backup now|list|restore`).

The only secret an operator handles is `OP_SERVICE_ACCOUNT_TOKEN`. Nothing is reached over
SSH: the programs run on Fargate, a shell is ECS Exec.

## Resetting the service database

`./deploy/run.sh --deployment staging db reset-db --yes-drop-database` dumps, drops and
recreates the database, and refuses while a subscription grants hosting: [DEPLOY.md](../DEPLOY.md),
"Resetting the service database".
