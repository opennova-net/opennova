# Deploying your own OpenNova NovaWorld instance

This repository deploys a complete NovaWorld stack (the gate, the NovaWorld
server, the legacy HTTP services, and the web portal) to your own cloud. Anyone
can stand up their own instance with the same commands the project maintainers
use. The original games reach your server through a hosts-file line that points
`gs.novaworld.net` at it (see DEVELOPING.md, "Test with retail Joint Operations").

## What you need

- **Docker** on the machine you deploy from. That is the only dependency. The
  deploy toolbox (1Password CLI, terraform, the docker client) runs in a
  container; the target host runs only docker.
- **A 1Password account** (any paid plan has service accounts) with a vault for
  this deployment.
- **An AWS account** and **a Cloudflare-managed domain** (optional but
  recommended for TLS).

No secret is ever committed to the repository or written to your disk outside a
container's in-memory tmpfs. The only secret you handle directly is a 1Password
service-account token.

## 1. Create the vault

Create a vault named `OpenNova-Deploy` (the service account comes in step 2; it
needs read + write access, the write for the terraform-state documents). Then
create these items (paste-ready):

```bash
op vault create OpenNova-Deploy

op item create --vault OpenNova-Deploy --title aws --category 'API Credential' \
  access_key_id=AKIA... secret_access_key=... region=us-east-1

op item create --vault OpenNova-Deploy --title cloudflare --category 'API Credential' \
  api_token=... zone_id=... domain=example.com

op item create --vault OpenNova-Deploy --title ghcr --category 'Secure Note' \
  owner=your-github-owner

op item create --vault OpenNova-Deploy --title app-prod --category 'Server' \
  admin_api_token="$(openssl rand -hex 24)" \
  admin_basic_auth_user=admin \
  admin_basic_auth_password="$(openssl rand -hex 16)" \
  expansion_publish_token="$(openssl rand -hex 24)"

# 1Password-generated SSH key the EC2 instance trusts.
op item create --vault OpenNova-Deploy --title ssh --category 'SSH Key'

# GitHub provisioning (infra/github): manages the expansion repos + their
# Actions secrets. token = a PAT with repo + admin:repo_hook scopes on the
# owner org; owner = the GitHub org/user that owns the expansion repos.
op item create --vault OpenNova-Deploy --title github --category 'API Credential' \
  token="ghp_replace_with_a_repo_admin_PAT" \
  owner=opennova-net
```

`app-prod`'s `expansion_publish_token` field and the `github` item feed only the
`infra/github` stack, which is pending retirement (ADR 0048, `TODO.md`); the
server no longer reads them.

The `op://` reference paths the toolbox reads are listed in
`deploy/env/terraform.env.tpl`, `deploy/env/app.prod.env.tpl` and
`deploy/env/github.tfvars.json.tpl`.

## 2. Mint the service-account token

The toolbox authenticates with a single 1Password **service account** — no
interactive login on the deploy host. Create one scoped to this vault with
**read + write** items (`write_items` is required so terraform state can be
stored as vault documents — for service accounts it covers creating items and
documents; there is no separate `create_items` permission):

```bash
# signed in to your own 1Password account as an owner/admin:
op service-account create opennova-deploy \
  --vault 'OpenNova-Deploy:read_items,write_items' --expires-in 90d
# prints the ops_... token ONCE — store it somewhere safe (e.g. a personal 1P item).
```

Web alternative: **Developer → Service Accounts → Create**, grant the
`OpenNova-Deploy` vault Read/Write, and copy the `ops_...` token. (Service
accounts need a paid 1Password plan with the feature enabled.)

Point the toolbox at the token and verify:

```bash
export OP_SERVICE_ACCOUNT_TOKEN=ops_...        # the only secret you handle
./deploy/run.sh secrets check                  # dry-runs every op:// reference
```

`secrets check` resolves every reference without printing a value. Fix any miss
before continuing (the `github.tfvars.json` line also needs the `expansions-ci`
item that step 3 creates, so on a first deployment it fails until then).

## 3. Stand up the infrastructure

First create the non-secret terraform var-file (gitignored). The toolbox injects
secrets from the vault as `TF_VAR_*`, but the non-secret knobs (bucket names,
instance size, region, the proxied toggle) come from this file:

```bash
cp infra/aws/terraform.tfvars.example infra/aws/terraform.tfvars
# edit downloads_bucket_name / backup_bucket_name etc. for your deployment
```

Then plan and apply:

```bash
./deploy/run.sh infra plan                     # review the plan
./deploy/run.sh infra apply                    # VPC, EC2, EIP, DNS, S3, CDN
```

Terraform state is stored as a 1Password document (`tfstate`), pulled before and
pushed after each run, so any operator with vault access can deploy. Review the
plan: after the first apply, pin `ami_id` in `infra/aws/terraform.tfvars` so AMI
drift never replaces your instance and releases the EIP.

Set `ONNET_PUBLIC_HOST` in `deploy/env/app.prod.env` to the EIP that
`infra apply` reports (`terraform output public_ip`).

`infra apply` also creates the `launcher_ci` IAM user (S3 upload to the
downloads bucket; pending retirement with the launcher, ADR 0048) and outputs its keys. Store them in the vault so the GitHub
stack (next step) can hand them to the expansion repos:

```bash
op item create --vault OpenNova-Deploy --title expansions-ci --category 'API Credential' \
  access_key_id="$(./deploy/run.sh infra output -raw ci_user_access_key_id)" \
  secret_access_key="$(./deploy/run.sh infra output -raw ci_user_secret_access_key)"
```

## 4. Provision the GitHub stack (expansion repos, pending retirement)

ADR 0048 removed the server's expansion publish callback and the web catalogue;
this stack stays only until the infrastructure retirement in `TODO.md`. Skip it
on a new deployment, and do not `github apply` on an existing one: it regenerates
`backend/seed/0002_expansions.generated.sql`, which the server can no longer seed.

The expansion content repos and their Actions secrets are managed by
`infra/github`. Because the repos already exist, adopt them once with an import,
then apply:

```bash
./deploy/run.sh github import                  # one time — adopt revx02/onjo01/ondx01
./deploy/run.sh github plan                    # should show no creates/destroys
./deploy/run.sh github apply                   # set the Actions secrets
```

State lives in its own 1Password document (`tfstate-github`). The Actions secrets
let each expansion repo's build workflow upload its package to S3 and call the
server's `/admin/internal/.../publish` endpoint (now removed). See
`infra/github/README.md` for the full pipeline and
`infra/github/expansion-publish-workflow.yml.example` for the workflow the
expansion repos copy in. On a brand-new GitHub org with no repos,
skip `github import` and run `github apply` directly.

## 5. Publish and deploy the images

The server and web images publish to GHCR from CI
(`.github/workflows/novaworld-images.yml`). Make those packages public once so
the target can pull them without credentials. Then:

```bash
./deploy/run.sh app deploy                     # pull GHCR images + compose up
./deploy/run.sh app status                     # ps on the remote host
./deploy/run.sh app logs novaworld             # tail the server
```

`app deploy` resolves the target IP from terraform output, reads the SSH key
from the vault into a tmpfs, and drives the remote docker engine over
`DOCKER_HOST=ssh://`. The host never needs anything but docker and sshd.

### The web build (game.<domain>)

The browser build of the game (ADR 0049) is a third image,
`ghcr.io/<owner>/opennova-game`, published by `.github/workflows/game-web.yml`
from `deploy/game/Dockerfile`: the wasm GDExtension, the Godot Web export and an
nginx on `:8090` that sends the COOP/COEP headers the threaded build needs. The
portal's nginx routes `game.<domain>` to it, `infra apply` creates the proxied
`game` record, and the security group keeps `:8090` private. Make the
`opennova-game` package public once, like the other two.

It publishes on its own path filter, so `--tag` does not move it:

```bash
./deploy/run.sh app deploy --game-tag sha-abc1234   # pin the game image; default latest
```

To run it locally, from the repo root with the submodules checked out:

```bash
docker build -f deploy/game/Dockerfile -t opennova-game .
docker run --rm -p 8090:8090 opennova-game          # http://localhost:8090
```

## 6. Backups

```bash
./deploy/run.sh backup now                     # sqlite .backup -> S3
./deploy/run.sh backup list
```

A backup sidecar also runs nightly (see `deploy/backup/`).
