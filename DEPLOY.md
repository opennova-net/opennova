# Deploying your own OpenNova NovaWorld instance

This repository deploys a complete NovaWorld stack (the gate, the NovaWorld
server, the legacy HTTP services, and the web portal) to your own cloud. Anyone
can stand up their own instance with the same commands the project maintainers
use. The original games keep working against your server through the launcher's
hosts-file redirection.

## What you need

- **Docker** on the machine you deploy from. That is the only dependency. The
  deploy toolbox (1Password CLI, terraform, the docker client) runs in a
  container; the target host runs only docker.
- **A 1Password account** (any paid plan has service accounts) with a vault for
  this deployment.
- **An AWS account** and **a Cloudflare-managed domain** (optional but
  recommended for TLS and the launcher's `nw.<domain>` anchor record).

No secret is ever committed to the repository or written to your disk outside a
container's in-memory tmpfs. The only secret you handle directly is a 1Password
service-account token.

## 1. Create the vault

Create a vault named `OpenNova-Deploy` and a service account with read (and
write, for terraform state) access to it. Then create these items (paste-ready):

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
  admin_basic_auth_password="$(openssl rand -hex 16)"

# 1Password-generated SSH key the EC2 instance trusts.
op item create --vault OpenNova-Deploy --title ssh --category 'SSH Key'
```

The `op://` reference paths the toolbox reads are listed in
`deploy/env/terraform.env.tpl` and `deploy/env/app.prod.env.tpl`.

## 2. Point the toolbox at your vault

```bash
export OP_SERVICE_ACCOUNT_TOKEN=ops_...        # the only secret you handle
./deploy/run.sh secrets check                  # dry-runs every op:// reference
```

`secrets check` resolves every reference without printing a value. Fix any miss
before continuing.

## 3. Stand up the infrastructure

```bash
./deploy/run.sh infra plan                     # review the plan
./deploy/run.sh infra apply                    # VPC, EC2, EIP, DNS, S3, CDN
```

Terraform state is stored as a 1Password document (`tfstate-prod`), pulled before
and pushed after each run, so any operator with vault access can deploy. Review
the plan: after the first apply, pin `ami_id` in `infra/aws/terraform.tfvars`
so AMI drift never replaces your instance and releases the EIP.

Set `ONNET_PUBLIC_HOST` in `deploy/env/app.prod.env` to the EIP that
`infra apply` reports (`terraform output public_ip`).

## 4. Publish and deploy the images

The server and web images publish to GHCR from CI (`.github/workflows/
novaworld-images.yml`). Make those packages public once so the target can pull
them without credentials. Then:

```bash
./deploy/run.sh app deploy                     # pull GHCR images + compose up
./deploy/run.sh app status                     # ps on the remote host
./deploy/run.sh app logs novaworld             # tail the server
```

`app deploy` resolves the target IP from terraform output, reads the SSH key
from the vault into a tmpfs, and drives the remote docker engine over
`DOCKER_HOST=ssh://`. The host never needs anything but docker and sshd.

## 5. Backups

```bash
./deploy/run.sh backup now                     # sqlite .backup -> S3
./deploy/run.sh backup list
```

A backup sidecar also runs nightly (see `deploy/backup/`).

## Staging

Every command takes `DEPLOY_ENV=staging`, which uses a separate terraform
workspace (no Elastic IP, `staging.` / `nw-staging.` DNS records) and a
`tfstate-staging` document, so you can rehearse the whole flow without touching
prod:

```bash
DEPLOY_ENV=staging ./deploy/run.sh infra apply
DEPLOY_ENV=staging ./deploy/run.sh app deploy
# ... smoke test ...
DEPLOY_ENV=staging ./deploy/run.sh infra destroy
```

## The launcher

Players install the launcher (published from CI to `downloads.<domain>`). It
redirects `gs.novaworld.net` to your server via a managed hosts-file block and
resolves your server IP from `GET /api/server-info`, so you do not hardcode it
into the launcher. See `launcher/README.md`.
