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
  admin_basic_auth_password="$(openssl rand -hex 16)"

# 1Password-generated SSH key the EC2 instance trusts.
op item create --vault OpenNova-Deploy --title ssh --category 'SSH Key'
```

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
# edit backup_bucket_name etc. for your deployment
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

The same file sets the website login's `ONNET_COOKIE_SECURE=1` and
`ONNET_TRUSTED_PROXIES=127.0.0.1` (apps/novaworld_server/README.md). The Secure
session cookie needs the site served over https only: keep the web records proxied
(`cloudflare_web_proxied`, the default) and turn on Cloudflare's "Always Use HTTPS",
or a visitor on plain http cannot stay logged in.

Behind the proxied records every request reaches nginx from a Cloudflare edge, so the
portal's nginx takes the visitor's address from `CF-Connecting-IP`, for a peer in
Cloudflare's published ranges only (`web/cloudflare-realip.conf`); the server's website
login and registration rate limits key on that address. Refresh the ranges before a
web-image release with `sh web/cloudflare-realip.sh` (it rewrites the file from
https://www.cloudflare.com/ips-v4 and /ips-v6, refusing any entry that is not a CIDR;
review and commit the diff). Cloudflare announces range changes ahead of time.

A Cloudflare peer does not make that header true: a Worker on anyone's zone can fetch
this origin (its IP is public) from a real Cloudflare edge with any `CF-Connecting-IP`
it likes, and so brake a chosen address on the website's login. The server keeps those
addresses' buckets apart from the retail login's (which keys on its own TCP peer), so a
forged address never brakes a game login; edge authentication, below, closes the
website side.

### Edge authentication (prepared, off)

With it on, a Cloudflare Transform Rule on our zone stamps a secret header,
`X-OpenNova-Edge`, on every request for the web hostnames, and the portal's nginx
answers `/api/` only to requests carrying it (403 otherwise, and the header is not
forwarded to the server), so only requests that came through our own zone, where
Cloudflare sets `CF-Connecting-IP` itself, reach the server. It stays off while
`EDGE_AUTH_SECRET` is unset (web/docker-entrypoint.sh, `infra/aws`
`cloudflare_ruleset.edge_auth`). To turn it on:

1. Add an `edge_auth_secret` field to the `app-prod` vault item: letters and digits, at
   least 32 (`openssl rand -hex 32`).
2. Give the Cloudflare API token in the `cloudflare` item the extra permission
   Zone / Transform Rules / Edit.
3. Add `TF_VAR_edge_auth_secret=op://OpenNova-Deploy/app-prod/edge_auth_secret` to
   `deploy/env/terraform.env.tpl` and run `./deploy/run.sh infra plan`, then
   `infra apply`: it creates the late-transform ruleset (a zone holds one per phase,
   so import an existing one first). The dashboard alternative: Rules, Transform
   Rules, Modify Request Header, "Set static" `X-OpenNova-Edge` to the secret for the
   root and `www` hostnames.
4. Add `EDGE_AUTH_SECRET=op://OpenNova-Deploy/app-prod/edge_auth_secret` to
   `deploy/env/app.prod.env.tpl` and deploy (`./deploy/run.sh app deploy`). The web
   container logs `edge authentication: ON`.
5. Check: `curl -i https://<domain>/api/health` answers 200, and the same request
   straight to the origin IP (`curl -i -H 'Host: <domain>' http://<EIP>/api/health`)
   answers 403.

Order matters: apply the rule (step 3) before deploying the secret (step 4), or the
site's API answers 403 in between. To rotate, change the vault field and run both
steps again.

To make the first site admin, register the account, then add a `bootstrap_admin` field
holding its username to the `app-prod` vault item and the line
`ONNET_BOOTSTRAP_ADMIN=op://OpenNova-Deploy/app-prod/bootstrap_admin` to
`deploy/env/app.prod.env.tpl`, and deploy. Never put the name in the committed
`app.prod.env`: a name in a public file is one anybody can register first. The server
promotes the account only while no account is an admin and logs what it did; remove
both lines after that deploy (the server warns at every boot while the setting is
present).

## 4. Publish and deploy the images

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
(manual runs only while the web build is paused) from `deploy/game/Dockerfile`: the wasm GDExtension, the Godot Web export and an
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

## 5. Backups

```bash
./deploy/run.sh backup now                     # sqlite .backup -> S3
./deploy/run.sh backup list
```

A backup sidecar also runs nightly (see `deploy/backup/`).

## Resetting the service database

The service's SQLite database is disposable. A schema fix edits a migration in
`backend/migrations` in place, and the server records the migrations it applied by
file name only, so an edited file never runs again on an existing database. A deploy
whose PR notes say "the service DB must be reset" (an edited migration) therefore
starts from an empty database:

```bash
./deploy/run.sh app reset-db --yes-drop-database --tag sha-abc1234
```

Without `--yes-drop-database` it prints what it would do and refuses. With it:

1. It pulls the images for `--tag` (and `--game-tag`, default `latest`, as in
   `app deploy`), so a mistyped tag fails before anything changes.
2. It stops `novaworld` and the backup sidecar. The portal stays up; its API fails
   until step 5.
3. It takes a final backup to S3, as `backup now`. If that fails it starts both
   services again and stops, having deleted nothing. `--no-backup` skips this step.
4. It deletes `novaworld.db` and its `-wal`, `-shm` and `-journal` files from the
   `nw_data` volume, through a one-off backup-sidecar container on that volume.
5. It deploys into a fresh `novaworld` container, then prints the server's
   `[boot] migrations:` line, which on the empty database reads `N applied, 0 skipped`;
   any other line, or none within a minute, makes it exit non-zero.

It deploys the tag itself rather than leaving the server stopped: the first server to
boot on the empty database applies and records every migration, so that boot must be
the image carrying the edit. An `app restart` or a deploy of an older tag in between
writes the old schema under the same file names, and nothing reports it. If step 5
fails, rerun `app deploy` with the same tag.

The final backup is on by default because the nightly one can be a day old; skip it
only when the database is known to be worthless or the backup path is what is broken.
Everything the database holds goes with the reset (accounts, website sessions, host
keys); the boot's seed puts back only the game rows. With no accounts left there is
no site admin either: make the first one again as section 3 describes
(`ONNET_BOOTSTRAP_ADMIN`). The backups from before the reset stay in S3 (the bucket
expires objects after 90 days) and hold the old schema: restoring one
(`deploy/backup/README.md`) undoes the reset.
