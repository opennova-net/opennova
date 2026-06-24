# PR 21 — NovaWorld production deploy + cutover (runbook)

Final step of the integration. Everything else is merged into the trunk
`web-nw-for-real-master` (the single integration PR **#136** → `master` is open), CI is green
except the two known-unrelated reds (`modsuperoed-smoke`, `validate-deliverables`, also red on
master), and the crypto is verified byte-exact vs retail (grill wave 3 NW-C1..C4, `edbb6c33`).

**This is operator-executed.** It needs your AWS account, the `OpenNova-Deploy` 1Password vault, a
Cloudflare-managed domain, and a docker-only host — it makes outward-facing infra + DNS changes.
Every command comes from `deploy/bin/on-deploy` and [`DEPLOY.md`](../DEPLOY.md).

Goal: deploy the stack (gate + NovaWorld server + legacy HTTP services + web portal) to your cloud,
smoke-test it against a retail JO client over the internet, and **merge #136**.

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
| `app-prod` | Server | `admin_api_token`, `admin_basic_auth_user`, `admin_basic_auth_password`, `expansion_github_token`, `expansion_publish_token` | server/web admin gate + expansion-publish pipeline |
| `ghcr` | Secure Note | `owner` | image pull `ghcr.io/<owner>/novaworld-{server,web}` |
| `github` | API Credential | `token`, `owner` | `infra/github` stack: manage the expansion repos + their Actions secrets |

- `aws`: an IAM access key that can manage VPC / EC2 / EIP / S3 / CloudFront / IAM in your account.
- `cloudflare`: `api_token` = a token with **Zone · DNS · Edit** on your zone; `zone_id` from the
  domain's Overview page; `domain` = your apex (e.g. `example.com`).
- `ssh`: a 1Password-generated **SSH Key** item — it auto-creates both `public key` and
  `private key` fields; nothing to fill in.
- You do **not** pre-create any terraform-state item: the toolbox stores state as the `tfstate`
  *document* on the first `infra apply` — which is why the service account below needs write
  access (for service accounts `write_items` covers creating items and documents).

### c. The service-account token (`ops_...`)
The toolbox authenticates with one 1Password **service account** scoped to the vault with
**read + write** (`write_items` covers the state document; there is no separate `create_items`
permission for service accounts). It is the only secret you ever handle:

```bash
op service-account create opennova-deploy \
  --vault 'OpenNova-Deploy:read_items,write_items' --expires-in 90d
# prints the ops_... token ONCE — store it somewhere safe.
export OP_SERVICE_ACCOUNT_TOKEN=ops_...
./deploy/run.sh secrets check        # green = every item/field path resolves before you spend on infra
```
(Web alternative: **Developer → Service Accounts → Create**, grant `OpenNova-Deploy`
Read/Write, copy the `ops_...` token. Service accounts need a paid 1Password plan.)

### d. GHCR packages public
The server+web images are built to GHCR by `.github/workflows/novaworld-images.yml`. Make the two
packages public once so the target host pulls them without credentials (else `app deploy` fails on
the remote pull).

## Deploy

Run from the repo root on your docker host:

```bash
export OP_SERVICE_ACCOUNT_TOKEN=ops_...
./deploy/run.sh secrets check        # dry-runs every op:// reference
./deploy/run.sh infra plan           # review: VPC, EC2, EIP, Cloudflare DNS (@/www/nw), S3, CDN
./deploy/run.sh infra apply          # applies it; state -> the tfstate 1P document
```

After the first apply:
- Pin `ami_id` in `infra/aws/terraform.tfvars` so AMI drift never replaces the instance (which
  would release the EIP).
- Set `ONNET_PUBLIC_HOST` in `deploy/env/app.prod.env` to the EIP (terraform output `public_ip`);
  the gate response advertises this host to clients.

Then deploy the app and verify:

```bash
./deploy/run.sh app deploy           # pull GHCR images + compose up over ssh://ubuntu@<EIP>
./deploy/run.sh app status           # ps on the box
./deploy/run.sh app logs novaworld   # tail the server boot
curl http://<EIP>:8080/api/server-info
./deploy/run.sh backup now           # sqlite .backup -> S3
./deploy/run.sh backup list
```

`infra apply` creates the Cloudflare records as part of the run: `nw.<domain>` (the launcher
anchor, unproxied, TTL 300) plus `@` / `www` (proxied TLS). Confirm `nw.<domain>` resolves to the
EIP and `/api/server-info` returns it. The database is fresh by design (no migration from a legacy
box); seed or register accounts anew.

## Smoke test — retail JO over the internet

Point a retail JO client at the new server and run the acceptance list. Either install the launcher
(published to `downloads.<domain>`) and let it manage the redirect to `nw.<domain>`, or for an
immediate check add a Windows hosts entry `gs.novaworld.net  <EIP>` by hand. Then:

- Gate probe answered; `NWStart` / `NWLogin` templates render.
- Register an account (web `/register` or server seed) and log in (EPASK `NAME` / `PASSWORD`).
- Server-browser rows live; **Host a Game** registers a row a second client sees.
- Two-client join; mid-match `GET /api/unknowns` shows `JOINTOPERATIONS` PN sightings.
- Quit host → GOODBYE / StopHosting removes the row.
- `GET /api/server-info` returns the server IP (the launcher's resolution contract).

If you are migrating off an older box, retire it once traffic is confirmed on the new one.

## Merge gate

Once the smoke passes, **merge PR #136** (`web-nw-for-real-master` → `master`).

## Gotchas

- **GHCR packages must be public** (or add a registry login) before `app deploy`.
- **tfstate lives in 1Password** (the `tfstate` document), pulled before and pushed after every
  `infra` run. Never run terraform outside the toolbox or you fork state.
- **Pin `ami_id`** after the first apply so a later apply does not replace the instance and drop the EIP.

## Verification (definition of done)

- Two retail clients complete login → browse → host → join → GOODBYE against the server;
  `/api/server-info` + `/api/unknowns` respond; `backup now` produces an S3 object (`backup list`
  shows it).
- `nw.<domain>` resolves to the EIP and the launcher connects through it.
- **#136 merged** to `master`.
