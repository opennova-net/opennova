# PR 21 — NovaWorld staging E2E + production cutover (runbook)

Final step of the integration. Everything else is merged into the trunk
`web-nw-for-real-master` (the single integration PR **#136** → `master` is open), CI is green
except the two known-unrelated reds (`modsuperoed-smoke`, `validate-deliverables`, also red on
master), and the crypto is verified byte-exact vs retail (grill wave 3 NW-C1..C4, `edbb6c33`).

**This is operator-executed.** It needs your AWS account, the `OpenNova-Deploy` 1Password vault, a
Cloudflare-managed domain, and a docker-only host — it makes outward-facing infra + DNS changes.
Every command comes from `deploy/bin/on-deploy` and [`DEPLOY.md`](../DEPLOY.md).

Goal: rehearse the whole stack on a throwaway **staging** environment against a retail JO client
over the internet, use that as the gate to **merge #136**, then **cut prod over** (fresh EC2+EIP,
deploy, Cloudflare DNS flip) and retire any old box.

## One-time operator setup

Provision these once. Paste-ready `op` commands are in [`DEPLOY.md`](../DEPLOY.md) §1–§2; the exact
schema the toolbox expects is below.

### a. The deploy machine — Docker only
The toolbox (op CLI + terraform + docker client) runs in a container (`deploy/run.sh` →
`deploy/docker-compose.deploy.yml`). Nothing else is installed locally; the EC2 target runs only
docker + sshd.

### b. The 1Password vault `OpenNova-Deploy`
Create the vault and these items. **Field names are exact** — the toolbox reads literal
`op://OpenNova-Deploy/<item>/<field>` paths (defined in `deploy/env/terraform.env.tpl` and
`deploy/env/app.prod.env.tpl`); a mismatched label fails `secrets check`.

| item | 1P category | fields (exact) | used for |
|---|---|---|---|
| `aws` | API Credential | `access_key_id`, `secret_access_key`, `region` | terraform AWS provider |
| `cloudflare` | API Credential | `api_token`, `zone_id`, `domain` | terraform Cloudflare DNS |
| `ssh` | SSH Key | auto-generated `public key`, `private key` | EC2 key pair (public) + `app deploy` over `ssh://` (private) |
| `app-prod` | Server | `admin_api_token`, `admin_basic_auth_user`, `admin_basic_auth_password` | server/web admin gate — **also used for staging** (the app env template is shared) |
| `ghcr` | Secure Note | `owner` | image pull `ghcr.io/<owner>/novaworld-{server,web}` |

- `aws`: an IAM access key that can manage VPC / EC2 / EIP / S3 / CloudFront / IAM in your account.
- `cloudflare`: `api_token` = a token with **Zone · DNS · Edit** on your zone; `zone_id` from the
  domain's Overview page; `domain` = your apex (e.g. `example.com`).
- `ssh`: a 1Password-generated **SSH Key** item — it auto-creates both `public key` and
  `private key` fields; nothing to fill in.
- You do **not** pre-create any terraform-state item: the toolbox stores state as the
  `tfstate-prod` / `tfstate-staging` *documents* on first `infra apply` — which is why the service
  account below needs write/create access.

### c. The service-account token (`ops_...`)
The toolbox authenticates with one 1Password **service account** scoped to the vault with
**read + write + create** (write/create for the state documents). It is the only secret you ever
handle:

```bash
op service-account create opennova-deploy \
  --vault 'OpenNova-Deploy:read_items,write_items,create_items' --expires-in 90d
# prints the ops_... token ONCE — store it somewhere safe.
export OP_SERVICE_ACCOUNT_TOKEN=ops_...
./deploy/run.sh secrets check        # green = every item/field path resolves before you spend on infra
```
(Web alternative: **Developer → Service Accounts → Create**, grant `OpenNova-Deploy`
Read/Write/Create, copy the `ops_...` token. Service accounts need a paid 1Password plan.)

### d. GHCR packages public
The server+web images are built to GHCR by `.github/workflows/novaworld-images.yml`. Make the two
packages public once so the target host pulls them without credentials (else `app deploy` fails on
the remote pull).

## Phase A — Staging rehearsal (separate workspace, no EIP, throwaway)

Run from the repo root on your docker host:

```bash
export OP_SERVICE_ACCOUNT_TOKEN=ops_...
./deploy/run.sh secrets check                          # dry-runs every op:// reference
DEPLOY_ENV=staging ./deploy/run.sh infra plan          # review: VPC, EC2, NO EIP, staging./nw-staging. DNS
DEPLOY_ENV=staging ./deploy/run.sh infra apply         # tfstate -> tfstate-staging 1P document
DEPLOY_ENV=staging ./deploy/run.sh app deploy          # pull GHCR images + compose up over ssh://ubuntu@IP
DEPLOY_ENV=staging ./deploy/run.sh app status          # ps on the box
DEPLOY_ENV=staging ./deploy/run.sh app logs novaworld  # tail the server boot
```

Set `ONNET_PUBLIC_HOST` in `deploy/env/app.prod.env` to the staging server's public IP before
`app deploy` (the gate response advertises this host to clients), then re-deploy.

### Staging smoke — retail JO over the internet
Point a retail JO client at the staging box. Simplest for a rehearsal: add a Windows hosts entry
`gs.novaworld.net  <staging-IP>` by hand (the launcher's managed hosts-block is itself a prod-time
thing to validate; see `launcher/README.md` to drive it instead). Validate the acceptance list:
- Gate probe answered; `NWStart`/`NWLogin` templates render.
- Register an account (web `/register` or server seed) and log in (EPASK `NAME`/`PASSWORD`).
- Server-browser rows live; **Host a Game** registers a row a second client sees.
- Two-client join; mid-match `GET /api/unknowns` shows `JOINTOPERATIONS` PN sightings.
- Quit host → GOODBYE/StopHosting removes the row.
- `GET /api/server-info` returns the staging IP (this is the launcher's resolution contract).

### Tear down
```bash
DEPLOY_ENV=staging ./deploy/run.sh infra destroy
```

## Phase B — Merge gate

Staging E2E green = the code/stack is validated end-to-end. **Merge PR #136**
(`web-nw-for-real-master` → `master`). The merge does **not** depend on prod being live.

## Phase C — Production cutover

```bash
./deploy/run.sh infra plan                             # default workspace = prod (EIP + shared singletons)
./deploy/run.sh infra apply                            # VPC, EC2, EIP, DNS, S3, CDN; tfstate-prod
# After first apply: pin ami_id in infra/aws/terraform.tfvars (AMI drift would replace the box + drop the EIP).
# Set ONNET_PUBLIC_HOST in deploy/env/app.prod.env to the EIP (terraform output public_ip).
./deploy/run.sh app deploy                             # fresh DB by design (no migration from the old box)
./deploy/run.sh app status
./deploy/run.sh backup now                             # sqlite .backup -> S3 (prod has the backup bucket)
./deploy/run.sh backup list
```

- Seed / register accounts anew on the fresh DB.
- **The DNS flip is the cutover**: terraform applies `cloudflare_record "nw"` (launcher anchor,
  unproxied, TTL 300) + `web_root`/`web_www` (proxied TLS). Confirm `nw.<domain>` resolves to the
  EIP and `/api/server-info` returns it.
- Re-run the smoke (Phase A list) against `nw.<domain>` via the published launcher
  (`downloads.<domain>`). Retire the old box once traffic is confirmed on the new one.

## Gotchas surfaced from the scripts

- **Backups are prod-only.** The S3 backup bucket is a default-workspace (prod) singleton in
  `infra/aws/main.tf`, so `DEPLOY_ENV=staging ... backup now` dies with "no backup_bucket_name
  output." Validate backups in Phase C, not staging.
- **Staging has no EIP** — its public IP changes on every `infra apply`; always run `infra apply`
  then `app deploy` in sequence (`app deploy` re-resolves the IP from terraform output).
- **tfstate lives in 1Password** (`tfstate-prod` / `tfstate-staging` documents), pulled before and
  pushed after every `infra` run. Never run terraform outside the toolbox or you fork state.
- **GHCR packages must be public** (or add a registry login) before `app deploy`.

## Verification (definition of done)

- **Staging:** two retail clients complete login → browse → host → join → GOODBYE against the
  staging box; `/api/server-info` + `/api/unknowns` respond. Record evidence in `plan/status.md`.
- **Prod:** same smoke against `nw.<domain>` through the published launcher; `backup now` produces
  an S3 object (`backup list` shows it); old box retired.
- **#136 merged** to `master`.
