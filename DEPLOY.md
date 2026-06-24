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
  expansion_github_token="ghp_replace_with_a_PAT_with_contents_write" \
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

`app-prod` carries two expansion secrets: `expansion_github_token` (a GitHub PAT
with `contents:write` on the expansion repos — the server pushes a release tag
with it) and `expansion_publish_token` (the bearer the expansion repo's CI
presents to the server's `/admin/internal/.../publish` callback; the same value
is set as an Actions secret on each expansion repo by the `infra/github` stack).

The `op://` reference paths the toolbox reads are listed in
`deploy/env/terraform.env.tpl` and `deploy/env/app.prod.env.tpl`.

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
before continuing.

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
downloads bucket) and outputs its keys. Store them in the vault so the GitHub
stack (next step) can hand them to the expansion repos:

```bash
op item create --vault OpenNova-Deploy --title expansions-ci --category 'API Credential' \
  access_key_id="$(./deploy/run.sh infra output -raw ci_user_access_key_id)" \
  secret_access_key="$(./deploy/run.sh infra output -raw ci_user_secret_access_key)"
```

## 4. Provision the GitHub stack (expansion repos)

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
server's `/admin/internal/.../publish` endpoint. See `infra/github/README.md` for
the full pipeline and `docs/net/expansion-publish-workflow.yml.example` for the
workflow the expansion repos copy in. On a brand-new GitHub org with no repos,
skip `github import` and run `github apply` directly.

## 5. Publish and deploy the images

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

## 6. Backups

```bash
./deploy/run.sh backup now                     # sqlite .backup -> S3
./deploy/run.sh backup list
```

A backup sidecar also runs nightly (see `deploy/backup/`).

## The launcher

Players install the launcher (published from CI to `downloads.<domain>`). It
redirects `gs.novaworld.net` to your server via a managed hosts-file block and
resolves your server IP from `GET /api/server-info`, so you do not hardcode it
into the launcher. See `launcher/README.md`.

### Cutting a launcher release

`.github/workflows/launcher-publish.yml` fires on a `launcher-v*` tag: it builds
the single-file `OpenNovaLauncher.exe`, attaches it to a GitHub Release, and — if
the AWS secrets/vars below are set — uploads the exe plus two manifests to
`downloads.<domain>/launcher/`: `version.json` (the launcher's AutoUpdater feed)
and `app.json` (the web download box on the landing page reads this).

One-time, wire the CI credentials from the infra output (reuse the launcher_ci
IAM user) + the bucket variable:

```bash
gh secret  set LAUNCHER_AWS_ACCESS_KEY_ID     -b "$(./deploy/run.sh infra output -raw ci_user_access_key_id)"
gh secret  set LAUNCHER_AWS_SECRET_ACCESS_KEY -b "$(./deploy/run.sh infra output -raw ci_user_secret_access_key)"
gh variable set DOWNLOADS_BUCKET -b downloads.opennova.net   # mandatory; DOWNLOADS_DOMAIN/AWS_REGION default OK
```

Then cut a release (the tag version must match `<Version>` in
`launcher/src/OpenNovaLauncher/OpenNovaLauncher.csproj` — currently `0.2.0`):

```bash
git tag launcher-v0.2.0 && git push origin launcher-v0.2.0
```

Once it runs, the landing page's download button appears automatically (it
fetches `downloads.<domain>/launcher/app.json`). To bump versions later, edit the
csproj `<Version>` first, commit, then tag the matching `launcher-v<x>`.

## Adding an expansion

Expansions are defined once in Terraform: `local.expansions` in
`infra/github/expansions.tf`. Add an entry, then `./deploy/run.sh github apply`
(`github import` first if the repo already exists). That one apply creates the
GitHub repo, sets its Actions secrets, and regenerates the catalogue seed
`backend/seed/0002_expansions.generated.sql` — commit the regenerated file so CI
bakes it into the server image. No C++/SQL/web edits; the server reads the
catalogue (and slug→repo mapping) from the DB, the web from `/api/expansions`.
See `infra/github/README.md` → "Add an expansion".

## Cutting an expansion release

Expansions appear in the web Expansions page and the launcher's Expansion Manager
as soon as their catalogue row is seeded (the Terraform-generated
`backend/seed/0002_expansions.generated.sql`) — but that's metadata only. The
downloadable file is written by **cutting a release**, which is separate. Until
you do, the launcher shows the expansion as "Not published yet" (Install disabled)
and the web page omits its Download button.

To publish one (e.g. the `onjo01` demo mod), with the admin token from the vault:

```bash
ADMIN=$(op read op://OpenNova-Deploy/app-prod/admin_api_token)
curl -X POST https://nw.<domain>/api/admin/expansions/onjo01/release \
  -H "Authorization: Bearer $ADMIN" -H 'Content-Type: application/json' \
  -d '{"version":"0.0.1"}'
```

That tags `onjo01-v0.0.1` on `opennova-net/onjo01`; the repo's publish workflow
packages its LFS content, uploads to `downloads.<domain>/expansion/onjo01/...`, and
calls back to `/admin/internal/expansions/onjo01/publish` which writes the
`expansion_files` row. Verify:

```bash
curl https://nw.<domain>/api/admin/releases -H "Authorization: Bearer $ADMIN"  # status: published
curl https://nw.<domain>/api/expansions                                         # onjo01 has files[].downloadUrl
```

Then the launcher (Refresh) shows it as installable. **Prerequisite:** the
`opennova-net/<slug>` repo must actually contain packageable content (a `.pff` at the
repo root, LFS-tracked); an empty repo produces an empty zip. The repo's workflow must
be the reconciled form (see `docs/net/expansion-publish-workflow.yml.example`) and its
Actions secrets come from `infra/github`.
