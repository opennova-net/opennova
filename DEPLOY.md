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
container's in-memory tmpfs, except what the Terraform state holds (the edge
authentication secret, once that is on; section 3). The only secret you handle
directly is a 1Password service-account token.

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
pushed after each run, so any operator with vault access can deploy. `infra apply` and
`infra destroy` push whatever state Terraform leaves, a failed run's too (it can have
changed resources and written a newer serial), then exit with Terraform's code.

Both directions read the vault's copy first and compare it with
`infra/aws/terraform.tfstate`. Identical files are left alone. Otherwise, without
`--force`:

- A pull refuses a local state with a higher serial than the vault's (an apply whose
  push never ran: `./deploy/run.sh state push` it), or with the same serial and other
  content.
- A push refuses when the vault's serial is the same or higher (another operator's
  apply: `state pull` it first).
- Both refuse two states of different lineages (Terraform's id for one state, kept
  through every serial): they are different states, not versions of one.

`state pull --force` and `state push --force` overwrite the other side once you know
which copy is right. The vault's document is found by its title in the vault's item
listing; only a vault with no `tfstate` document reads as a first run (a pull) or as
one to create (a push), and any other vault failure stops the command, so nothing runs
on a state of unknown age and no push makes a second document.

Review the plan: after the first apply, pin `ami_id` in `infra/aws/terraform.tfvars` so AMI
drift never replaces your instance and releases the EIP.

Set `ONNET_PUBLIC_HOST` in `deploy/env/app.prod.env` to the EIP that
`infra apply` reports (`terraform output public_ip`).

The same file sets the website login's `ONNET_COOKIE_SECURE=1`,
`ONNET_TRUSTED_PROXIES=127.0.0.1` and `ONNET_API_PROXY_ONLY=1`
(apps/novaworld_server/README.md). The Secure session cookie needs the site served
over https only: keep the web records proxied (`cloudflare_web_proxied`, the default)
and turn on Cloudflare's "Always Use HTTPS", or a visitor on plain http cannot stay
logged in.

The server's own HTTP port (8080) is open to the internet: a stock client fetches the
`NW*.dll` pages and the `.gsb` server lists from it directly, through no proxy. Its
`/api/` is the website's, though, and `ONNET_API_PROXY_ONLY=1` answers it only to the
portal's nginx, which reaches the server from `127.0.0.1` (host networking, with
`host.docker.internal` mapped to the loopback in `docker-compose.prod.yml`): any other
peer gets 403, so the website's API is reached only through nginx and the checks it
applies. Check after a deploy: `curl -i http://<EIP>:8080/api/health` answers 403, and
`curl -i http://<EIP>:8080/jop_2.gsb` still answers 200.

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
forwards `/api/` only for requests carrying it (403 otherwise, and the header is not
forwarded to the server), so the requests nginx passes on came through our own zone,
where Cloudflare sets `CF-Connecting-IP` itself. It guards nginx only; the server's own
port refuses `/api/` to anyone but nginx through `ONNET_API_PROXY_ONLY`, above. It stays
off while `EDGE_AUTH_SECRET` is unset (web/docker-entrypoint.sh, `infra/aws`
`cloudflare_ruleset.edge_auth`). Terraform creates the rule only while the web records
are proxied (`cloudflare_web_proxied`), and its plan warns about a secret without them:
unproxied records reach the origin through no Transform Rule, so with
`EDGE_AUTH_SECRET` set nginx would refuse every `/api/` request. Keep the web records
proxied while it is on. To turn it on:

1. Add an `edge_auth_secret` field to the `app-prod` vault item: lowercase hex, at
   least 32 characters (`openssl rand -hex 32` gives 64). The entrypoint and Terraform
   accept letters and digits, but nginx compares the header without regard to case,
   so mixed case adds no strength.
2. Give the Cloudflare API token in the `cloudflare` item the extra permission
   Zone / Transform Rules / Edit.
3. Add `TF_VAR_edge_auth_secret=op://OpenNova-Deploy/app-prod/edge_auth_secret` to
   `deploy/env/terraform.env.tpl` and run `./deploy/run.sh infra plan`, then
   `infra apply`: it creates the late-transform ruleset. A zone holds one entrypoint
   ruleset per phase, so if the zone has one already, import it first; Terraform is
   then authoritative over all of its rules, and the apply replaces whatever rules it
   held with this one (copy any you need into `infra/aws/main.tf` before applying).
   The dashboard alternative: Rules, Transform Rules, Modify Request Header, "Set
   static" `X-OpenNova-Edge` to the secret for the root and `www` hostnames.
4. Add `EDGE_AUTH_SECRET=op://OpenNova-Deploy/app-prod/edge_auth_secret` to
   `deploy/env/app.prod.env.tpl` and deploy (`./deploy/run.sh app deploy`). The web
   container logs `edge authentication: ON`.
5. Check: `curl -i https://<domain>/api/health` answers 200, the same request
   straight to the origin's nginx (`curl -i -H 'Host: <domain>' http://<EIP>/api/health`)
   answers 403, and so does the server's own port (`curl -i http://<EIP>:8080/api/health`).

Order matters: apply the rule (step 3) before deploying the secret (step 4), or the
site's API answers 403 in between.

The secret lands in the Terraform state in plain text (`sensitive` only keeps it out of
plan output): in the vault's `tfstate` document, and in `infra/aws/terraform.tfstate`,
which every `infra` run pulls into the working copy the toolbox mounts and leaves there
(gitignored, but on your disk). Treat the `tfstate` document as a secret, and delete the
local file after a run on any machine others can read.

**Rotating.** nginx accepts a second value, `EDGE_AUTH_SECRET_PREVIOUS`, beside the
current one while a rotation runs, so the rule can move without a window of 403s:

1. In one sitting, with no other app deploy or `infra` run in between (from the vault
   edit until this deploy, the vault names a secret nothing accepts yet, so any of
   them would answer the site's API with 403): in the `app-prod` vault item, copy the
   current `edge_auth_secret` into a new field `edge_auth_secret_previous` and set
   `edge_auth_secret` to the new secret (`openssl rand -hex 32`); add
   `EDGE_AUTH_SECRET_PREVIOUS=op://OpenNova-Deploy/app-prod/edge_auth_secret_previous`
   to `deploy/env/app.prod.env.tpl`; and deploy the app. nginx now accepts both (the
   web container logs that the previous secret is accepted too); the rule still
   stamps the old one.
2. `./deploy/run.sh infra apply`: the rule stamps the new secret.
3. Remove the `EDGE_AUTH_SECRET_PREVIOUS` line (and the vault field) and deploy the app
   again: only the new secret passes. Run the turn-on checks (step 5 above).

The previous secret takes the same rule as the current one (the container stops on a
malformed one, one set without `EDGE_AUTH_SECRET`, or one equal to it).

**Turning it off.** Unset the environment first, then remove the rule, or the site's
API answers 403 in between: remove the `EDGE_AUTH_SECRET` (and any
`EDGE_AUTH_SECRET_PREVIOUS`) line from `deploy/env/app.prod.env.tpl` and deploy the app
(the web container logs `edge authentication: OFF`), then remove the
`TF_VAR_edge_auth_secret` line from `deploy/env/terraform.env.tpl` and run
`infra apply`, which deletes the ruleset.

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

Another image, `ghcr.io/<owner>/opennova-serve`, publishes from its own workflow
(`.github/workflows/serve-image.yml`, `apps/serve/Dockerfile`): the headless game server
the hosted game servers run (ADR 0053), with no retail byte in it
(`apps/serve/README.md`, "Container image"). The target never pulls it: each hosted
server's machine does, at the digest `app deploy` pins (`--serve-tag`, section 6), and
`--tag` does not move it. Make the `opennova-serve` package public once, like the server
and web images.

### The web build (game.<domain>)

The browser build of the game (ADR 0049) is a fourth image,
`ghcr.io/<owner>/opennova-game`, published by `.github/workflows/game-web.yml`
(manual runs only while the web build is paused) from `deploy/game/Dockerfile`: the wasm GDExtension, the Godot Web export and an
nginx on `:8090` that sends the COOP/COEP headers the threaded build needs. The
portal's nginx routes `game.<domain>` to it, `infra apply` creates the proxied
`game` record, and the security group keeps `:8090` private. Make the
`opennova-game` package public once, like the server and web images.

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

## 6. Hosted game servers

An account an admin approved (`can_host` in the admin users page) can start one
headless Joint Operations server from the site's My server page (ADR 0053). The service
launches one EC2 machine per server (`infra/aws/hosted.tf`, `c7a.medium` by default),
running the public `opennova-serve` image over the game data, a private encrypted EBS
snapshot made once from your licensed install and mounted read-only. The server lists
itself on this NovaWorld through the service's public address, its owner administers it
from the site (the service is the only client of its admin port, 32800, open to the
service's security group alone; owners never get RAT access), and its stop terminates
the machine. Machines are terminated, never stopped, so every start has a new public
address. No machine has an SSH key; break-glass is SSM Session Manager.

How the service launches, watches and ends the machines, the bootstrap route and the
machine script are in `apps/novaworld_server/README.md`, "Hosted servers".

Hosting stays off until a data snapshot exists: with no `hosted` item in the vault,
`infra apply` creates nothing for it and changes nothing but its outputs, and `app
deploy` runs the service with `ONNET_HOSTED_PROVIDER=none`. `app deploy` reads the
infra's `hosted_enabled` output (it pulls the state first), so after this lands run
`./deploy/run.sh infra apply` once before the next deploy: with hosting off it only adds
the outputs. A state without them stops the deploy rather than guess hosting off;
`--no-hosting` deploys without reading them.

### The game data

The snapshot holds the files `opennova-serve` reads from the install, and nothing else:

| File | Why |
|---|---|
| `resource.pff`, `localres.pff`, `language.pff` | the retail boot table it mounts (`engine/base/vfs/vfs.h`) |
| `expansion/jox01/jox01.pff`, `expansion/jox01/jox01L.pff` | Escalation, for a server started with it (`/exp jox01`) |
| `expansion/jox01/version.txt` | the expansion checksum the session advertises to joiners |
| `score.ini` (when present) | the scoring table a server reads loose from the install |
| `_NSTMOUT.TXT` (when present) | the session timeout override, read when it is there |

The movies, the music, the executables and the player's files stay home. A JO:CA install
gives about 1 GB, a 2 GiB volume. The snapshot is encrypted with the account's default
EBS key (`aws/ebs`) and private to the account; it never goes in git or an image, and
players never reach it (the machines mount it read-only and run nothing but the server).

### Turning it on

1. **Make the data snapshot, once.** Mount your JO:CA folder into the toolbox and run
   `hosted data-snapshot`. From PowerShell:

   ```powershell
   $env:OP_SERVICE_ACCOUNT_TOKEN = "ops_..."
   docker compose -f deploy\docker-compose.deploy.yml run --rm -T `
     -v "D:/SteamLibrary/steamapps/common/Joint Operations Combined Arms:/hosted-data:ro" `
     deploy hosted data-snapshot
   ```

   or `$env:HOSTED_DATA_DIR = "<folder>"; .\deploy\run.ps1 hosted data-snapshot`, or from
   Git Bash `HOSTED_DATA_DIR="D:/SteamLibrary/steamapps/common/Joint Operations Combined
   Arms" ./deploy/run.sh hosted data-snapshot`.

   The toolbox takes the files above from the folder (the one place it reads them) and
   streams them into a helper image it builds on your docker (`deploy/hosted-data`),
   which makes an ext4 file system of them labelled `opennova-jo` and writes it straight
   into a new snapshot through the EBS direct APIs: no EC2 machine, no S3 bucket, no SSH.
   The snapshot is the only AWS resource it makes, and it deletes it if anything fails;
   the copy of the files lives only in the helper's container, which goes when it ends.
   It prints the snapshot id (`snap-...`). The vault's AWS credentials need
   `ebs:StartSnapshot`, `ebs:PutSnapshotBlock`, `ebs:CompleteSnapshot`,
   `ec2:DescribeSnapshots` and `ec2:DeleteSnapshot`, and `kms:DescribeKey`, with which
   the helper confirms the snapshot's key is the AWS-managed `aws/ebs` (without it, it
   warns). It refuses any other key, because the service's launch policy grants no KMS
   action: an account whose default EBS key is customer-managed resets it first
   (`aws ec2 reset-ebs-default-kms-key-id`). The `aws/ebs` key's own policy admits the
   account's principals through EBS, so an admin deploy user needs nothing more. A
   deploy user restricted by an SCP or a key policy without that statement would also
   need `kms:GenerateDataKeyWithoutPlaintext`, `kms:Decrypt` and `kms:CreateGrant` on
   the key, as AWS's EBS direct APIs guide lists. This is untested here.

2. **Record it in the vault.** Create an item `hosted` in the deploy vault with a field
   `data_snapshot_id` holding the id. Every `infra` run reads it as
   `TF_VAR_hosted_data_snapshot_id`; with no item titled `hosted` in the vault's listing
   hosting is off, and any other failure (the listing, or reading the item's field)
   stops the run, so a vault error never plans hosting away. The plan
   reads the snapshot (its size becomes the data volume's), so a mistyped or deleted id
   fails at `infra plan`.

3. **Check the vCPU quota.** The machines and the server instance share the account's
   On-Demand Standard instances quota: 5 `c7a.medium` (1 vCPU each) beside the
   `c7i.large` server (2) is 7 vCPUs, and a new account's quota can be lower.

   ```bash
   ./deploy/run.sh aws service-quotas get-service-quota --service-code ec2 --quota-code L-1216C47A
   ```

   `Quota.Value` must be at least 2 + `ONNET_HOSTED_CAP` (7 at the default cap);
   request more in the Service Quotas console if it is not. A launch past it fails with
   `VcpuLimitExceeded`, which the server's timeline shows as a failed launch.

   While you are in the console, an [AWS Budgets](https://console.aws.amazon.com/billing/home#/budgets)
   cost alarm (say at the monthly figure under "Cost" below) is the AWS-side backstop
   to the service's own cap.

4. **Plan, then apply.**

   ```bash
   ./deploy/run.sh infra plan
   ```

   The plan must create the hosted resources alone: `aws_launch_template.hosted`,
   `aws_security_group.hosted` and its three rules, `aws_iam_role.hosted_machine` with
   its Session Manager policy (`aws_iam_role_policy.hosted_machine_ssm`) and instance
   profile, and `aws_iam_policy.hosted_service` with its attachment to the existing
   service role; update `aws_security_group.server` in
   place (one ingress rule: the machines' bootstrap fetch on 8080, from their security
   group); and add outputs. It must not touch `aws_instance.server` (it holds the
   database). Then `./deploy/run.sh infra apply`.

5. **Deploy the app.**

   ```bash
   ./deploy/run.sh app deploy --tag sha-<server> --serve-tag sha-<serve>
   ```

   With the infra's `hosted_enabled` output true, `app deploy` gives the server
   `ONNET_HOSTED_PROVIDER=ec2` and every setting the provider needs from the Terraform
   outputs: the region, the launch template, the subnet and its range (the only peers
   the bootstrap route answers), the deployment tag, the server instance's private
   address, and the serve image pinned to the digest `--serve-tag` names at that moment
   (default `latest`; pick a tag from the "Serve image" workflow). The digest lookup and
   every machine's pull are anonymous, so the `opennova-serve` GHCR package must be
   public and stay public: a private package fails the deploy here, and a package made
   private later fails every launch at its pull. The server logs
   `[boot] hosted servers: ec2 in <region>, ...` and the ranges its bootstrap route
   answers; a setting it lacks turns hosting off with a boot warning naming it. `app
   reset-db` sets the same.

   Optional, in `deploy/env/app.prod.env`: `ONNET_HOSTED_CAP` (the most servers at once,
   default 5) and `ONNET_HOSTED_MISSIONS` (the rotation a start takes when it names none,
   and the maps the site's picker offers).

6. **Approve the first account** (the admin users page, `can_host`) and run the smoke
   test below.

### The smoke test

1. Start a server from My server. Its timeline runs `requested`, `provisioning`,
   `booting` (the machine boots, installs docker, mounts the data and fetches its
   bundle: a few minutes), `listing`, then `running`.
   `./deploy/run.sh hosted status` lists the machine. Note how long `booting` and
   `listing` took (the timeline's times): the service gives a machine 10 minutes to
   boot and 5 to list (`Ec2Provider::kBootTimeoutMs`, `kListingTimeoutMs`), and real
   times near either want those raised.
2. The server shows in the OpenNova and the retail server browsers, at the machine's
   public address. A retail client and an OpenNova client both join. Run `/PROFILE` to
   check the tick holds on the machine type.
3. The site's console works: the player list, chat, a map change.
4. A crash: open a shell on the machine (`aws ssm start-session --target <machine>`,
   which needs the Session Manager plugin where you run it) and
   `sudo docker kill opennova-serve`. The machine powers off and terminates, and the
   server's timeline shows the crash and a restart on a new machine (restart 1 of 3).
5. Restart the service (`./deploy/run.sh app restart novaworld`): the running server
   resumes on its machine and lists again.
6. Stop it from My server: the machine terminates (`hosted status` shows it
   `shutting-down`, then nothing).
7. The bootstrap route answers no one outside the VPC:
   `curl -i -X POST http://<EIP>:8080/hosted/bootstrap/1` answers 404.
8. The server cannot read the machine role: on the machine (SSM),
   `sudo setpriv --reuid=10001 --regid=10001 --clear-groups curl -sS -m 3 http://169.254.169.254/`
   fails to connect (the machine script checks the same before the server starts, and
   ends the machine when it does not hold).

### Cost

One always-on `c7a.medium` is about $0.05 an hour, about $37 a month, plus its public
IPv4 address (about $3.60 a month) and its volumes (a 10 GiB root and the 2 GiB data
volume, about $1 a month). The snapshot costs a few cents a month. Egress can add as much
again as the compute: a full 64-player server sends about 1.8 GB an hour. At the cap of 5
always-on servers, budget about $200 a month and more under load. `ONNET_HOSTED_CAP`
lowers the cap.

### The kill switch

The admin fleet view's **stop all** (`POST /api/admin/hosted/stop-all`) moves every live
server to `stopping`, and the service terminates each machine. Then, to keep hosting off
across deploys, deploy with `./deploy/run.sh app deploy --no-hosting ...`.

If the service is down, end the machines by hand:
`./deploy/run.sh hosted status` lists them, and
`./deploy/run.sh aws ec2 terminate-instances --instance-ids <machine> ...` ends them. A
machine the service finds with no live server is ended at its next boot or sweep anyway.

To turn hosting off for good: stop all, wait until `hosted status` lists nothing, deploy
with `--no-hosting`, delete the `hosted` vault item, then `infra apply` (it removes the
hosted resources; later deploys find hosting off on their own). The snapshot stays until
you delete it (`./deploy/run.sh aws ec2 delete-snapshot --snapshot-id <id>`).

### Operating notes

- **A new machine image.** The launch template keeps the Ubuntu image it was made with
  (it ignores later image changes, as the server instance does). To move new machines to
  Canonical's newest: `./deploy/run.sh infra apply -replace='aws_launch_template.hosted[0]'`,
  then `app deploy` (the template's id changes). Running machines keep theirs.
- **New data.** Run `hosted data-snapshot` again, put the new id in the vault, `infra
  apply` (the template moves to it), and delete the old snapshot. New machines take it.
- **A machine's logs.** `aws ec2 get-console-output --instance-id <machine> --latest`
  (through `./deploy/run.sh aws`) shows the bootstrap's steps and the server's end, also
  after the machine is gone, but only while EC2 still lists the terminated machine
  (about an hour): read a crash's console soon. On a live machine, over SSM,
  `journalctl -u opennova-serve` and `/var/log/cloud-init-output.log`.
- **A refused call.** The service logs each AWS CLI failure with its message (`app logs
  novaworld`, lines `[hosted] ...`); the timeline the owner sees says only "the launch
  failed". For an `UnauthorizedOperation`,
  `./deploy/run.sh aws sts decode-authorization-message --encoded-message <message>`
  names the statement or condition that refused it.
- **The machine side never restarts the server.** The unit has `Restart=no`, and
  `SuccessAction`/`FailureAction=poweroff`: whatever ends `opennova-serve` (any exit
  code, a signal) ends the machine, and the machine gives the code no meaning. The
  service alone makes and counts every crash restart (3, forgiven after an hour of
  running), each on a new machine with new credentials.
- **The machine role.** It holds the five actions Session Manager needs and nothing
  else, and the server (UID 10001, on the machine's own network) is fenced off the
  instance metadata service by an `iptables` rule, so it never holds even those.
- **The offline infra check.** `infra/aws/tests/hosted.tftest.hcl` plans and applies the
  hosted resources against mock providers (no credentials, nothing reaches AWS); the
  file says how to run it, and the "Infra" workflow runs it, with `terraform fmt` and
  `validate` and shellcheck on the toolbox and the machine script, on every change
  under `infra/` or `deploy/`.

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
(`ONNET_BOOTSTRAP_ADMIN`). If that setting is still in the vault-injected secrets when
you reset, remove it first: on the empty database its username is free, and whoever
registers it first is made admin at the next boot. Register the account yourself, then
set it and deploy once.

The backups from before the reset stay in S3 (the bucket expires objects after 90
days). Each one records every migration it ran, so undoing the reset takes both steps:
restore a pre-reset backup (`deploy/backup/README.md`) AND deploy the server tag that
ran before the reset, which the plan prints as `novaworld now:`. A newer server on a
restored backup skips every migration and runs on the old schema, and nothing reports
it.
