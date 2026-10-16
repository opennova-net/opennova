# Deploying your own OpenNova NovaWorld instance

This repository deploys a complete NovaWorld stack to AWS, behind Cloudflare: the gate,
the NovaWorld server, the legacy HTTP services, the web portal, a PostgreSQL database,
and the fleet of machines the hosted game servers run on.
Anyone can stand up their own instance with the commands the project maintainers use.
The original games reach your server through a hosts-file line that points
`gs.novaworld.net` at its Elastic IP (the OpenNova Switcher writes it; DEVELOPING.md,
"Test with retail Joint Operations").

The stack in one paragraph: one disposable EC2 host (`c7i.large`) runs the service in
docker compose and keeps no state of its own. Its database is RDS PostgreSQL 17 in
private subnets; its secrets live in SSM Parameter Store, copied from a 1Password vault;
its releases are bundles in an S3 bucket; its logs go to CloudWatch. Deploys and every
command on the host go through SSM (Run Command, Session Manager): no SSH anywhere. The
retail ports (gate UDP 7597, NovaWorld UDP 64206, HTTP 8080) stay open on one Elastic IP
that no Terraform plan can release; the website reaches the host through Cloudflare alone,
over TLS Cloudflare verifies. Hosted game servers run on machines the service launches
itself: shared machines holding up to 12 servers, and own machines (a paid add-on)
holding one.

## What you need

- **Docker** on the machine you deploy from. That is the only dependency: the deploy
  toolbox (1Password CLI, Terraform, the AWS CLI, the docker client) runs in a container.
- **A 1Password account** (any paid plan has service accounts) with a vault for this
  deployment.
- **An AWS account.**
- **A Cloudflare-managed domain.** The website is served through Cloudflare's proxy only:
  the origin admits Cloudflare's address ranges alone and presents a Cloudflare Origin CA
  certificate.
- **A Stripe account** for the monthly support tiers (the paid hosting). Without one the
  stack runs with billing off: nobody can subscribe, and only an admin's comp grant lets
  an account host servers.

No secret is ever committed to the repository or written to your disk outside a
container's in-memory tmpfs, except what the Terraform state holds (the edge
authentication secret and the origin certificate's key; section 3). The only secret you
handle directly is a 1Password service-account token.

## 1. Create the vault

Create a vault named `OpenNova-Deploy`; the service account comes in step 2 (it needs read
and write access: the write for the generated secrets and the Terraform state
documents). Create the items you supply yourself:

```bash
op vault create OpenNova-Deploy

op item create --vault OpenNova-Deploy --title aws --category 'API Credential' \
  access_key_id=AKIA... secret_access_key=... region=us-east-1

op item create --vault OpenNova-Deploy --title cloudflare --category 'API Credential' \
  api_token=... zone_id=... domain=example.com

op item create --vault OpenNova-Deploy --title ghcr --category 'Secure Note' \
  owner=your-github-owner
```

The Cloudflare token (`cloudflare/api_token`: My Profile, API Tokens, Create Token,
Custom token) needs exactly these, the zone ones for the one zone and the account one
for its account:

| Scope | Permission | What needs it (`infra/aws/edge.tf`, `email.tf`) |
|---|---|---|
| Zone | DNS / Edit | the site's records |
| Zone | Zone Settings / Edit | SSL/TLS "Full (strict)", Always Use HTTPS, the minimum TLS version; turning Email Routing on |
| Zone | SSL and Certificates / Edit | the origin certificate (Origin CA) |
| Zone | Zone WAF / Edit | the Stripe webhook's skip rule (custom rules) |
| Zone | Transform Rules / Edit | edge authentication's header |
| Zone | Email Routing Rules / Edit | `support@<domain>`'s rules |
| Account | Email Routing Addresses / Edit | the email destination |
| Zone (optional) | Bot Management / Read | reading Bot Fight Mode's state through the API (below) |

**Bot Fight Mode must be off.** The zone is on the Free plan, whose Bot Fight Mode no
WAF rule can skip (the webhook's skip rule covers the managed rules, rate limiting and
the legacy products, not it): with it on, Cloudflare may challenge Stripe's webhook
POSTs, and a challenged webhook never arrives. In the dashboard, Security, Bots: the
"Bot Fight Mode" toggle is off. With the optional permission, `GET
https://api.cloudflare.com/client/v4/zones/<zone id>/bot_management` answers
`"fight_mode": false`. The end-to-end check is Stripe's "Send test webhook" answering
2xx ("Monthly support tiers (Stripe)").

Everything else is made by the toolbox once the token works (step 2):
`./deploy/run.sh secrets init` adds whatever is missing and never overwrites a field.
It prints item and field names, never a value:

| Item | Fields | What `secrets init` puts there |
|---|---|---|
| `app-prod` (`app-<deployment>` for another deployment) | `admin_api_token`, `admin_basic_auth_user`, `admin_basic_auth_password`, `database_password` | random values (`admin` for the user) |
| `app-prod` | `edge_auth_secret` (the zone's one secret, for every deployment) | 64 random hex digits |
| `rds` (`rds-<deployment>`) | `master_password` | random |
| `stripe-test`, `stripe-live` | `secret_key`, `webhook_secret`, `price_tier1`, `price_tier2`, `price_tier3`, `price_own_machine` | `REPLACE_ME` |
| `alerts` | `email` | `REPLACE_ME` |
| `email` | `forward_to` | `REPLACE_ME` |

Fill the `REPLACE_ME` fields in the 1Password app (that keeps the keys out of shell
history): `alerts/email` is where every alarm, budget and cost anomaly writes;
`email/forward_to` is the mailbox `support@<domain>` forwards to ("Email", section 3);
in each Stripe item you fill `secret_key` (the Stripe dashboard) and `./deploy/run.sh
stripe setup` the rest ("Monthly support tiers (Stripe)", below). Optional fields the
toolbox picks up when present: `app-prod/bootstrap_admin` (section 3) and
`app-prod/edge_auth_secret_previous` (a rotation). Item `hosted` holds the hosted
servers' data snapshots (section 6). There is no SSH item any more.

The `op://` references the toolbox reads are listed in `deploy/env/terraform.env.tpl`
(Terraform's), `deploy/env/aws.env.tpl` (the toolbox's own AWS calls) and
`deploy/env/runtime.env.tpl` (the service's runtime secrets).

## 2. Mint the service-account token

The toolbox authenticates with a single 1Password **service account**, with no
interactive login. Create one scoped to this vault with **read + write** items
(`write_items` covers creating items and documents):

```bash
# signed in to your own 1Password account as an owner/admin:
op service-account create opennova-deploy \
  --vault 'OpenNova-Deploy:read_items,write_items' --expires-in 90d
# prints the ops_... token ONCE: store it somewhere safe (e.g. a personal 1P item).
```

Web alternative: **Developer, Service Accounts, Create**, grant the `OpenNova-Deploy` vault
Read/Write, and copy the `ops_...` token. Then:

```bash
export OP_SERVICE_ACCOUNT_TOKEN=ops_...        # the only secret you handle
./deploy/run.sh secrets init                   # the generated secrets and the placeholders
./deploy/run.sh secrets check                  # every reference, the Stripe items
```

`secrets check` resolves every reference without printing a value, and reports each
Stripe item as `OK (test mode, 3 tiers + own machine)`, `placeholders: <fields> (billing
off)`, `absent (billing off)` or `FAILED: <field> (malformed)` / `(a test key in a live
item)`; a failed one makes it exit non-zero. The forms it checks: `secret_key`
`sk_<mode>_...` or `rk_<mode>_...` (the item's mode: `test` for `stripe-test`, `live` for
`stripe-live`), `webhook_secret` `whsec_...`, each price `price_...` (a comma list: the
first is sold, the rest are grandfathered). It also says whether `alerts/email` and
`email/forward_to` are still placeholders (an absent `email` item fails: every
Terraform run reads it, so run `secrets init` once after an upgrade).

`./deploy/run.sh` (or `.\deploy\run.ps1`) rebuilds the toolbox image when
`deploy/Dockerfile` changed and runs it with the repository mounted. The toolbox is a
Debian image with pinned, checksummed releases of every tool.

## 3. Stand up the infrastructure

### Deployments

A deployment is one complete stack: `prod` (the default) or another such as `staging`.
Every command takes `--deployment NAME` (or `OPENNOVA_DEPLOYMENT=NAME`), which picks:

| | prod | another (staging) |
|---|---|---|
| Terraform workspace | `default` | `staging` |
| state document (vault) | `tfstate` | `tfstate-staging` |
| var-file | `infra/aws/prod.tfvars` | `infra/aws/staging.tfvars` |
| SSM parameters | `/opennova/prod/...` | `/opennova/staging/...` |
| vault items | `app-prod`, `rds`, `stripe-live` | `app-staging`, `rds-staging`, `stripe-test` |
| site | `example.com`, `www` | `staging.example.com` |
| Elastic IP | adopted (below) | its own |

The account-wide pieces belong to prod: the S3 account public access block, the
operations bucket (every deployment's releases and dumps), the Cloudflare zone's
settings and rulesets, the budgets, the cost anomaly monitor, the deploy workflow's
role. Run prod first.

### The var-file

The non-secret knobs live in `infra/aws/<deployment>.tfvars`:

```bash
cp infra/aws/deployment.tfvars.example infra/aws/prod.tfvars
```

It holds no secret, so committing it keeps every operator on the same knobs. Never name
it `terraform.tfvars` or `*.auto.tfvars` (or their `.json` forms): Terraform loads those
into every workspace (staging would take prod's Elastic IP), so the toolbox refuses to
run while one exists in `infra/aws/`.

For prod set `service_eip_allocation_id` to the allocation id of the Elastic IP retail
clients use (`./deploy/run.sh aws ec2 describe-addresses`). Terraform reads that address
and associates it, and never manages it: no plan can release it. Prod never makes an
address of its own, so a prod plan without the allocation id stops at a precondition (a
brand-new prod stack allocates one first, `./deploy/run.sh aws ec2 allocate-address
--domain vpc`, and adopts it). Another deployment (staging) gets a managed address of
its own and never adopts one: its plan stops too when an allocation id reaches it. Pin
`azs` once the stack exists so a change in the region's list never moves a subnet.

### Plan and apply

```bash
./deploy/run.sh infra plan                     # review the plan
./deploy/run.sh infra apply                    # plans again, applies exactly that plan
```

`infra apply` saves its plan to a file and applies that file, never a plan made again.
It refuses a plan that destroys or replaces what no apply makes again or what the
service runs on: an Elastic IP or its association, the service instance or its network
interface, the database, a bucket. Read the plan, then `infra apply --allow-destroy` if
that is meant. A plan that would make prod a new Elastic IP is refused whatever the
flags. On prod it then prints what the plan does (`N to create, N to update, N to
replace...`) and applies the saved plan only once you type `yes`; `--yes` answers for
you, and a run with no terminal (a script) refuses without it. The database also carries
`prevent_destroy` and deletion protection. Further arguments pass through (`infra apply
--allow-destroy -replace=aws_instance.service`). Other `infra` commands:
`state-rm ADDRESS` (forget a resource, keep its object), `import ADDRESS ID`, `output`,
`list`, and `destroy` (refused for prod).

What it makes: the VPC (`10.44.0.0/16`: public subnets a/b for the service and the
machines, private a/b with no route out for the database), the service instance with its
fixed network interface (`10.44.0.10`) and the adopted Elastic IP, its security group
(the retail ports to everyone, 443 to Cloudflare's ranges alone, the internal port to
the machines alone, no 22, no 80), the database, the SSM parameters and converge
document, the operations bucket, the logs, metrics and alarms, the budgets, the
Cloudflare records, the origin certificate, the zone's TLS setting, edge authentication
and the webhook rule, the zone's Email Routing (with a destination), and (with data
snapshots) the fleet's launch templates. A new database takes 10 to 15 minutes.

The state is a 1Password document per deployment, pulled before and pushed after each
run, so any operator with vault access can run the stack. `infra apply`, `state-rm` and
`import` push whatever state Terraform leaves, a failed run's too, then exit with
Terraform's code. Every pull or push records, beside the state, the vault document this
checkout is in step with (`<state file>.vault-sha256`, gitignored), and both directions
compare three ways, so diverged states are refused rather than overwritten:

- **Both changed since the last sync:** pull and push both refuse. Merge by hand
  (`terraform state rm`/`import` until this checkout holds both, then `state push --force`),
  or keep one side whole with `state push --force` or `state pull --force`.
- **Only the local state changed** (an apply whose push never ran): a pull refuses;
  `./deploy/run.sh state push` it.
- **Only the vault's changed** (another operator's push): a push refuses; `state pull`.
- **No record of the last sync:** the serials decide, and both say they cannot tell.
- **Different lineages:** both refuse; they are different states.

The state holds the edge authentication secret and the origin certificate's private key
in plain text: treat the state documents as secrets, and delete the local state files
after a run on a machine others can read. The database's master password never enters
the state (Terraform 1.11 write-only arguments): it reaches RDS at creation, and again
only when `db_master_password_version` in the var-file is bumped.

### Secrets to the host

```bash
./deploy/run.sh secrets sync                   # vault -> SSM SecureStrings
```

`secrets sync` reads each reference in `deploy/env/runtime.env.tpl` from the vault and
writes it to `/opennova/<deployment>/secret/<NAME>`, each value going vault, tmpfs file,
`put-parameter --value file://...`: never an argument, never printed. The Stripe fields
are synced together, and only while the deployment's Stripe item (`stripe-live` for
prod, `stripe-test` otherwise) is complete and well formed; with placeholders billing
stays off and none of them is synced. Fields marked optional (`bootstrap_admin`,
`edge_auth_secret_previous`) are synced while they exist; `secrets sync --prune` deletes
from SSM what the vault no longer names. The host reads them at its next converge
(`app converge`, or the next `app deploy`), into a memory-only env file.

### The database

After the first apply, make the service's role and its database:

```bash
./deploy/run.sh db bootstrap
```

It runs on the host (SSM), in a one-off `postgres:17-alpine` container on the host's
network, as the RDS master user: it creates the role `novaworld` with the
`DATABASE_PASSWORD` secret as its password (so `secrets sync` first), and the database
`novaworld` owned by it, `TEMPLATE template0 ENCODING 'SQL_ASCII' LC_COLLATE 'C'
LC_CTYPE 'C'` (the only kind the server accepts: retail hosts send cp1252 bytes, which it
stores byte for byte). It is idempotent: run again, it only sets the role's password
again (how a changed `database_password` takes effect). The master password reaches the
host as a short-lived SecureString the command deletes when it ends. TLS is required
(`rds.force_ssl`), and every client verifies the server (`sslmode=verify-full` against
the pinned RDS CA bundle).

### Cloudflare: TLS to the origin

The records are proxied; Cloudflare connects to the origin's nginx on 443 and verifies
its certificate, a Cloudflare Origin CA certificate (ECDSA, the domain and its
first-level names, 15 years) that Terraform makes and hands the host through SSM
(`/opennova/<deployment>/tls/*`); the web container's entrypoint writes it to a tmpfs.
The zone is set to SSL/TLS "Full (strict)" with "Always Use HTTPS". Apply the zone
setting together with an app deploy whose nginx serves 443: in between, the site answers
52x. There is no port 80 on the origin. The retail HTTP port (8080) has nothing to do
with this: stock clients speak plain HTTP to it, and `ONNET_API_PROXY_ONLY=1` refuses its
`/api/` to anyone but the portal's nginx.

Behind the proxied records every request reaches nginx from a Cloudflare edge, so nginx
takes the visitor's address from `CF-Connecting-IP`, for Cloudflare's published ranges
only (`web/cloudflare-realip.conf`; refresh it before a web-image release with `sh
web/cloudflare-realip.sh`, review and commit the diff). The security group takes the
same ranges live from Cloudflare at each plan.

### Edge authentication

A Cloudflare Worker on anyone's zone can fetch an origin from a real Cloudflare edge with
any `CF-Connecting-IP` it likes. Edge authentication closes that: a Transform Rule on our
zone stamps `X-OpenNova-Edge: <secret>` on every request for the web hostnames (every
deployment's: `edge_other_deployments`), and nginx forwards `/api/` only for requests
carrying it. It is on: `secrets init` makes `app-prod/edge_auth_secret`, Terraform makes
the rule, and the prod overlay requires `EDGE_AUTH_SECRET`, so a web container without it
never starts. A zone holds one entrypoint ruleset per phase: import an existing
late-transform ruleset before the first apply, and Terraform then owns all its rules.

**Rotating.** nginx accepts a second value, `EDGE_AUTH_SECRET_PREVIOUS`, during a
rotation, so the rule moves without a window of 403s:

1. In one sitting: in `app-prod`, copy `edge_auth_secret` into a new field
   `edge_auth_secret_previous` and set `edge_auth_secret` to a new value (`openssl rand
   -hex 32`); `secrets sync`; `app converge` (for each deployment). nginx accepts both.
2. `./deploy/run.sh infra apply`: the rule stamps the new secret.
3. Delete the `edge_auth_secret_previous` field, `secrets sync --prune`, `app converge`.

### Email (support@<domain>)

Mail to `support@<domain>` (and to each local part in `email_routes`, default
`["support"]`) reaches one mailbox of yours through Cloudflare Email Routing
(`infra/aws/email.tf`, prod's alone: it is the zone's):

1. In the vault, item `email`, set `forward_to` to the destination address (`secrets
   init` makes the field as `REPLACE_ME`; `REPLACE_ME` or empty keeps email routing off).
   Not an address at the domain itself, which would loop.
2. `./deploy/run.sh infra apply`: it turns Email Routing on for the zone (Cloudflare adds
   and locks its MX and SPF records; a zone holding MX records of another mail service
   must drop them first), adds the destination address to the zone's account, and makes
   one rule per local part.
3. Cloudflare emails the destination a verification link: click it. Nothing is forwarded
   until then (Email, Email Routing, Destination addresses in the dashboard shows it
   pending).

It is receive-only: replies leave from the destination mailbox, under its own address.
Sending as `support@<domain>` needs an outbound mail provider (an SMTP relay with the
domain's SPF and DKIM), which this stack does not set up. Every address no rule names is
refused (the zone's catch-all stays off). The token's email permissions are in section 1.

### The first site admin

Register the account on the site, then add a `bootstrap_admin` field holding its
username to the deployment's app item, `secrets sync` and `app converge`. The server
promotes the account only while no account is an admin and logs what it did. Then
delete the field, `secrets sync --prune` and `app converge` (the server warns at every
boot while the setting is present). Never put the name in a committed file: a name in a
public file is one anybody can register first.

## 4. Deploy

The images publish to GHCR from CI: `novaworld-server` and `novaworld-web`
(`.github/workflows/novaworld-images.yml`) and `opennova-serve` (the hosted servers,
`serve-image.yml`; it also carries hostd, the machines' agent). Make each package public
once: the host and the machines pull anonymously.

```bash
./deploy/run.sh app deploy --tag sha-abc1234 --serve-tag sha-def5678
```

`app deploy`:

1. resolves each image to its digest now: `--tag` sets the server and web images
   (`--server-tag` and `--web-tag` set one alone), `--serve-tag` and `--hostd-tag` the
   others. An image given no tag keeps the current release's digest, so a web-only
   release (`--web-tag`) never restarts the NovaWorld server and its lobby sessions; the
   first release takes `latest` for any tag not given;
2. builds a release bundle: the compose files, the committed `deploy/env/app.prod.env`,
   `release.env` (the digests), the backup sidecar's build context, and a manifest (the
   checkout's revision, the images, the previous release); it parses the compose files
   first, so a broken one fails before anything changes;
3. uploads it to `s3://<ops bucket>/releases/<deployment>/<id>.tar.gz`, points
   `/opennova/<deployment>/release` at it, and sends the converge document to the host;
4. prints the converge's output: the images it pulled, the containers it recreated (only
   those whose image or settings changed), the server's `[boot] migrations:` line, and
   the containers' state. It waits for the server to answer `/healthz`.

The host runs the same converge at every boot, so a new host serves the current release
with no step of yours. Downtime per server change is the server's restart, a few
seconds.

```bash
./deploy/run.sh app releases                   # the newest 20, the current one starred
./deploy/run.sh app rollback                   # converge the previous release
./deploy/run.sh app rollback --to <id>         # or a named one
./deploy/run.sh app converge                   # the current release again (new secrets)
./deploy/run.sh app status                     # the containers (over SSM)
./deploy/run.sh app restart novaworld          # one service (all without a name)
./deploy/run.sh app logs novaworld --follow    # CloudWatch, one stream per service
./deploy/run.sh app shell                      # a root shell on the host (Session Manager)
```

On prod, `app restart` and `app down` ask first: they go ahead once you type `yes`, or
with `--yes`, and a run with no terminal refuses without it (as `infra apply`, `fleet
end` and `fleet roll` do). `--no-hosting` deploys with hosted game servers off (the kill
switch, section 6). The
release bundles never expire, so any release can be rolled back to; a rollback of the
server across a migration is the database's business (section 5).

### The deploy workflow (optional)

`.github/workflows/deploy.yml` runs the same `app deploy` from GitHub Actions, approval
gated, with no vault: set `github_deploy = true` in `prod.tfvars` and `infra apply`
(GitHub's OIDC provider and a role trusting only this repository's `production`
environment); in the repository settings make the `production` environment with
required reviewers, and the variables `AWS_DEPLOY_ROLE_ARN` (the infra output
`github_deploy_role_arn`) and `AWS_REGION`. Run it from the Actions tab with the tags; it
deploys prod from the commit it runs on. The role may read the deployment's settings,
upload its release bundles, point its release and send the converge document to its
service instance, and nothing else.

## 5. Backups

Two independent layers:

- **RDS point-in-time recovery**, 14 days, kept even past a deletion of the instance
  (and a final snapshot at one). `./deploy/run.sh db snapshot` takes a manual snapshot
  now (before risky work); RDS restores a point in time or a snapshot into a new
  instance (the RDS console, or `aws rds restore-db-instance-to-point-in-time`), which you
  then adopt by pointing the stack at it.
- **pg_dump**, the backup sidecar (`deploy/backup`): a custom-format dump to S3 nightly
  at 03:00 UTC, kept 90 days, independent of the instance. It runs on the host's network
  with the server's `DATABASE_URL` and `DATABASE_PASSWORD`; the dumps go under
  `novaworld/` (prod) or `novaworld-<deployment>/` in the operations bucket.

```bash
./deploy/run.sh backup now                     # a dump now
./deploy/run.sh backup list                    # the dumps
./deploy/run.sh backup restore novaworld/novaworld-<ts>.dump --yes-overwrite-database
```

`backup restore` stops the server, dumps the database as it stands (a
`novaworld-pre-restore-<ts>` copy), restores the dump in one transaction (`pg_restore
--clean --if-exists --single-transaction`), and starts the server, whose boot applies any
migration the dump predates. A server older than the dump refuses an edited or unknown
migration rather than run on it.

**The restore drill**, once a quarter: restore the newest prod dump into staging
(`--deployment staging backup restore novaworld/novaworld-<ts>.dump` after copying the
object to `novaworld-staging/`), sign in on the staging site and check an account and a
CD key.

### Resetting the service database

Once billing is live the database is durable: migrations are append-only, and `db
reset-db` refuses while any subscription that grants hosting (active, trialing, past_due)
is recorded:

```bash
./deploy/run.sh db reset-db --yes-drop-database
```

It dumps the database first (`--no-backup` skips it), stops the server, drops the
database and makes it again (the same `CREATE DATABASE` as `db bootstrap`), and starts
the server, whose first boot applies every migration; it prints the `[boot] migrations:`
line. `--orphan-subscriptions` resets anyway: those customers keep paying with no
account linked here, so cancel them in Stripe first. Everything goes with the reset:
accounts, sessions, host keys, CD keys, DFX2 activations, billing links; make the first
site admin again (section 3).

## 6. Hosted game servers

An account with a hosting entitlement (a monthly support tier, or an admin's comp grant
of `comp_servers`) starts servers from the site's My servers page, picking a data set:
the game folder the server mounts, named by the games slug of the lobby it lists in
(`jop_2_consumer`, `dfx2_consumer`, `opennova_0_consumer`). The server is always the
public `opennova-serve` image, started with `/game <title>`.

The service places each server on a machine it launches itself (its EC2 machine driver,
with the host's role):

- **shared machines**, `t3a.large` with unlimited CPU credits, holding up to 12 servers
  (`shared_slots`) of any accounts, packed best-fit; an idle one ends after 30 minutes
  (`fleet_idle_minutes`);
- **own machines**, `c7a.medium`, exactly one server: the own-machine add-on.

Slot k of a machine is game UDP `32768+k` (to players) and admin TCP `32800+k` (to the
service instance alone); hostd, the agent on each machine, runs one container per slot
and reports to the service's internal listener (`10.44.0.10:8070`, admitted from the
machines alone). Every machine mounts every data set, read-only. Machines are terminated,
never stopped. No machine has an SSH key or database credentials; break-glass is Session
Manager. At most 2 shared and 2 own machines run at once (`fleet_max_shared`,
`fleet_max_own`); `ONNET_HOSTED_CAP` (default 24) caps the servers.

### The game data

Each data set's snapshot holds the files `opennova-serve` reads from an install, and
nothing else:

| File | Data sets | Why |
|---|---|---|
| `resource.pff`, `localres.pff`, `language.pff` | every one (required) | the retail boot table it mounts |
| `expansion/jox01/jox01.pff`, `expansion/jox01/jox01L.pff` | `jop_2_consumer` (required) | Escalation (`/exp jox01`) |
| `expansion/jox01/version.txt` | `jop_2_consumer` (required) | the expansion checksum joiners compare |
| `score.ini`, `_NSTMOUT.TXT` (when present) | `jop_2_consumer`, `dfx2_consumer` | the scoring table; the session timeout override |

The snapshots are encrypted with the account's default EBS key (`aws/ebs`), private to
the account, and never go in git or an image. Each file system carries its data set's
label, by which the machine mounts it.

### Turning it on

1. **Make each data set's snapshot, once.** Mount its game folder into the toolbox:

   ```bash
   HOSTED_DATA_DIR="D:/SteamLibrary/steamapps/common/Joint Operations Combined Arms" \
     ./deploy/run.sh hosted data-snapshot --data-set jop_2_consumer
   ```

   (PowerShell: `$env:HOSTED_DATA_DIR = "<folder>"; .\deploy\run.ps1 hosted data-snapshot
   --data-set jop_2_consumer`.) It streams the files into a helper image
   (`deploy/hosted-data`) that writes an ext4 image straight into a new snapshot through
   the EBS direct APIs, and prints its id. The vault's AWS credentials need
   `ebs:StartSnapshot`, `ebs:PutSnapshotBlock`, `ebs:CompleteSnapshot`,
   `ec2:DescribeSnapshots`, `ec2:DeleteSnapshot` and `kms:DescribeKey`.

2. **Record them in the vault.** Item `hosted`, one field per data set,
   `data_snapshot_id_<slug>`. With no `hosted` item hosting is off; a broken item stops
   every `infra` run rather than plan hosting away.

3. **Check the vCPU quota.** The service instance and the machines share the account's
   On-Demand Standard instances quota (L-1216C47A): the host's `c7i.large` 2, each
   `t3a.large` 2, each `c7a.medium` 1. The defaults need 8 (2 + 2 x 2 + 2 x 1); a staging
   stack doubles it. Request 32 in the Service Quotas console to leave room for a roll:

   ```bash
   ./deploy/run.sh aws service-quotas get-service-quota --service-code ec2 --quota-code L-1216C47A
   ```

4. **Plan, then apply.** The plan adds the two launch templates
   (`aws_launch_template.fleet["shared"]`, `["own"]`), the machines' security group and its
   rules, the machine role and the service role's fleet policy, and changes infra.env
   (the service's `ONNET_HOSTED_*` settings, among them `ONNET_HOSTED_DATA_SETS`: the data
   sets the machines mount, which are the ones the site offers). A data set's default map
   rotation for new servers is `hosted_data_set_missions` in the deployment's tfvars
   (`{ jop_2_consumer = ["m01.bms", ...] }`); without one, an owner picks the missions.

5. **Deploy**, so the service reads them and pins the serve and hostd images:
   `./deploy/run.sh app deploy --serve-tag sha-<serve> --hostd-tag sha-<serve>`. The
   server logs `[boot] hosted servers: ec2 ...`.

6. **Subscribe, or grant.** An account hosts once it subscribes to a support tier, or an
   admin gives it a comp grant (`comp_servers`, the admin user page). Run the smoke test
   below.

### Fleet commands

```bash
./deploy/run.sh fleet status                   # EC2's view, then the service's
./deploy/run.sh fleet drain <machine>          # no new server there; its servers move
./deploy/run.sh fleet end <machine>            # end it (its servers move first)
./deploy/run.sh fleet roll                     # move every server to new machines
```

They call the service's admin API on the service instance over SSM, from the loopback,
with the admin token. `fleet roll` after a new machine image or a new hostd: servers move
without counting restarts. On prod `fleet end` and `fleet roll` ask first (`--yes`
answers for you).

### The smoke test

1. Start a server from My servers. Its timeline runs requested, provisioning (a machine
   launching: a few minutes the first time), booting, listing, running.
2. It lists in its game's lobby in the OpenNova and the retail server browsers, at the
   machine's address and its slot's port; a retail client and an OpenNova client both
   join. A second account's server lands on the next slot of the same shared machine and
   is joinable too.
3. The site's console works (players, chat, a map change), and the server's stats move.
4. `app restart novaworld`: the running servers keep running and list again.
5. Stop one: its slot frees; with the machine empty, it ends after the idle time.
6. The kill switch (below) stops them all.

### The kill switch

The admin fleet view's **stop all** stops every live server and the service ends the
machines. To keep hosting off across deploys, deploy with `app deploy --no-hosting`. If
the service is down, `fleet status` lists the machines and `./deploy/run.sh aws ec2
terminate-instances --instance-ids <machine> ...` ends them; the service ends any machine
it finds without a server at its next pass anyway. To turn hosting off for good: stop all,
deploy with `--no-hosting`, delete the `hosted` vault item, `infra apply`.

### Operating notes

- **A new machine image.** The launch templates keep the image they were made with:
  `./deploy/run.sh infra apply -replace='aws_launch_template.fleet["shared"]'
  -replace='aws_launch_template.fleet["own"]'`, then `fleet roll`.
- **New data, or a new data set.** Make its snapshot, set its vault field, `infra apply`
  (the templates mount it), `fleet roll`, then delete the old snapshot.
- **A machine's logs.** CloudWatch `/opennova/<deployment>/machines`, and the serial
  console (`aws ec2 get-console-output --instance-id <machine> --latest`) while EC2 still
  lists it.
- **A refused AWS call.** The service logs each failure (`app logs novaworld`); for an
  `UnauthorizedOperation`, `./deploy/run.sh aws sts decode-authorization-message
  --encoded-message <message>` names the condition that refused it.

## Monthly support tiers (Stripe)

Billing runs on Stripe Checkout and the Customer Portal: the server makes the sessions
and redirects to Stripe, so no card data and no publishable key touch our pages. Test
mode serves staging (`stripe-test`), live mode prod (`stripe-live`).

**What exists.** Both modes are set up (made 2026-10-15, by what `stripe setup` below
now does): the products and their monthly USD prices, found by lookup key
(`opennova_tier1_monthly` $5, `opennova_tier2_monthly` $10, `opennova_tier3_monthly`
$15: 1, 2 and 3 hosted servers; `opennova_own_machine_monthly` $45, the add-on that puts
one of the account's servers on a machine of its own, about what a `c7a.medium` costs),
their ids in the item's `price_tier1`..`price_tier3` and `price_own_machine` fields; the
Customer Portal configuration tagged `opennova=support_tiers`; and the webhook endpoints,
live `https://opennova.net/api/billing/webhook` and test
`https://staging.opennova.net/api/billing/webhook`, each on the API version the server
pins (`2025-03-31.basil`) with the 11 events it handles (`checkout.session.completed`,
`.expired`; `customer.subscription.created`, `.updated`, `.deleted`, `.paused`,
`.resumed`; `invoice.paid`, `.payment_failed`, `.payment_action_required`;
`customer.deleted`), its signing secret in the item's `webhook_secret`.

**The toolbox.** The one field you fill is the item's `secret_key` (Developers, API
keys); the rest is the toolbox's:

```bash
./deploy/run.sh stripe check --mode test       # read alone: the item and its objects
./deploy/run.sh stripe setup --mode test       # make what is missing (idempotent)
./deploy/run.sh stripe check --mode live
```

`--mode` defaults to the deployment's (prod live, the others test); test mode names
staging's site, live mode prod's. `stripe setup` makes what the mode lacks and leaves
the rest: a product and a monthly price per lookup key (`--tier1`, `--tier2`, `--tier3`,
`--own-machine` take other amounts in US dollars: an amount that differs from the current
price makes a new price, which takes the lookup key and goes first in the field, the old
one kept after it, since the first is sold and the rest still grant); the price ids
into the item; the portal configuration (customers switch among the three tier products,
an upgrade prorating at once and a downgrade waiting for the period's end, quantity
changes off; cancel any subscription at the period's end, a tier or one own machine,
since each own machine is a subscription of its own (quantity 1) bought through
Checkout; and update the card, the email and the address and see invoices; the
privacy and terms links are `https://<domain>/privacy` and `/terms`, the return link
`https://<web host>/me/support`), brought to those settings when it exists; and the
webhook endpoint on `https://<web host>/api/billing/webhook`, whose missing events it
adds, a new one's signing secret going into `webhook_secret` (an endpoint's API version
never changes: for another, delete the endpoint in Stripe and run setup again).
`stripe check` reads the same and fails on what keeps billing from working: the key's
form and mode, the webhook secret's form, each price (active, monthly, of the item's
mode, the lookup key's), the portal configuration, the endpoint (enabled, its API
version, its events). Neither prints a key or a signing secret: the key reaches curl
through a config file in the tmpfs, the values reach the vault through a template file.

The key `setup` uses needs write on Products, Prices, Customer portal and Webhook
Endpoints; the server's key (the same field) needs Customers, Checkout Sessions, Customer
portal and Subscriptions write, Prices and Products read. A restricted key
(`rk_test_...`, `rk_live_...`) with both sets serves both; Stripe's 403 names a missing
permission.

**The portal and the default configuration.** The server opens portal sessions without
naming a configuration, so a customer gets the account's default one (Settings, Billing,
Customer portal). `stripe setup` and `stripe check` say whether the tagged configuration
is the default; while it is not, set the default to the same features in the dashboard,
or have the server name the tagged one.

**Dashboard only** (no API makes these):

1. **Activate the account** (live mode needs the site's legal pages live: terms,
   privacy, refunds). Statement descriptor: `OPENNOVA`.
2. **Public details** (Settings, Public details), in each mode: the business name, the
   support email `support@opennova.net` (section 3, "Email"), the website
   `https://opennova.net`, the terms of service URL `https://opennova.net/terms` and the
   privacy policy URL `https://opennova.net/privacy`. Checkout asks every buyer to accept
   the terms, and Stripe refuses every Checkout session while no terms URL is set: the
   site answers 503 `stripe_unavailable` and the server logs Stripe's message
   (`checkout -> no session: rejected 400 ...`). The portal configuration's own terms and
   privacy links (`stripe setup`) do not set these.
3. **Revenue recovery** (Billing, Revenue recovery): retries (Smart Retries), then cancel
   the subscription when all retries fail. A past-due account keeps its servers running
   but starts no new one; they stop when Stripe cancels. Its customer emails: receipts
   for successful payments, and the failed-payment emails.
4. **Tax** is optional: Stripe Tax with `ONNET_BILLING_AUTOMATIC_TAX=1`. EU VAT applies
   from the first EU sale; check with an accountant.

Then `./deploy/run.sh secrets check` (the item reads `OK`), `secrets sync`, `app
converge`: the server logs billing on; with an incomplete set it logs "billing off".
**Check delivery**: the endpoint's "Send test webhook" must answer 2xx. A challenge page
means Cloudflare stopped it: the webhook's custom rule skips the WAF's rules
(`infra/aws/edge.tf`), but Bot Fight Mode cannot be skipped by a rule, so keep it off
(section 1). On a plan without some skippable phase or product the apply names it: drop
it from `cloudflare_webhook_skip_phases` / `_products` in the var-file.

## GitHub

The GitHub organization `opennova-net` is managed by `infra/github`, all of it: the
organization's settings, members and Actions permissions, the CI runners and their runner
group, every repository (settings, topics, default branch, vulnerability alerts),
`opennova`'s `master` protection and its Actions secrets. A change made in GitHub's
settings is drift: the next `github plan` shows it, and `github apply` puts it back. Make
the change in `infra/github` instead. The repositories are never destroyed by Terraform
(`prevent_destroy`; one dropped from the map on purpose is archived, not deleted).

CI's build and test legs run on two larger GitHub-hosted runners: `windows-16core` and
`ubuntu-16core` (16 cores, 64 GB). They run the images of `windows-latest` and
`ubuntu-latest`, so they share compiler caches with the jobs that stay on those. They sit
in the runner group `opennova-ci`, which only `opennova` may use; it allows public
repositories, which GitHub's default group does not. `.github/workflows/ci.yml` names them
in `runs-on`. A pull request from a fork runs on the standard runners, so only the
organization's own branches spend minutes. Larger runners are billed per minute even for a
public repository, and `maximum_runners` (10 Windows, 5 Linux) caps how many machines run
at once.

The stack is the organization's, not a deployment's: it has no workspaces, the toolbox
runs it without `--deployment`, and its state is the vault document `tfstate-github-org`
(`tfstate-github` was the retired expansion stack's; leave it alone). What stays out of
this public repository comes from the vault:

- **`github-terraform`**, fields:
  - `token`: a fine-grained personal access token, resource owner **opennova-net**, with
    organization permissions **Administration**, **Members** and **Self-hosted runners**
    (each Read and write) and, on **All repositories**, **Administration** and **Secrets**
    (Read and write).
  - `billing_email`: the organization's billing email.
  - `members`: the members and their roles as JSON, `{"login": "admin", "other": "member"}`.
    Every member keeps the membership private. A login left out is removed from the
    organization at the next apply, and a new one is invited.
- **`reference-assets`**, field `token`: the value of the `REFERENCE_ASSETS_TOKEN` secret,
  which the retail legs use to mount the two private reference repositories
  (`docs/asset-gated-tests.md`). A fine-grained token, resource owner **opennova-net**,
  access to only `opennova-reference-assets` and `opennova-reference-retail-packed`,
  **Contents: Read-only**. When it expires, put the new token in the item and `github
  apply`: Terraform keeps the value in its state and overwrites a secret set by hand.

An org admin makes the tokens, or approves them if the organization reviews fine-grained
tokens. Then:

```bash
./deploy/run.sh github plan
./deploy/run.sh github apply            # shows the plan, then asks
./deploy/run.sh github output           # the runners' names
```

`infra/github/imports.tf` adopted what existed before the stack. A new repository, member
or secret is a change to the stack: add it, plan, apply. A new image replaces a runner.

## Operating

- **Logs.** `./deploy/run.sh app logs [novaworld|web|backup] [--follow] [--since 2h]`
  reads CloudWatch (`/opennova/<deployment>/service`, 30 days). On the host, `docker logs`
  works as well (`app shell`).
- **Alarms** go to the SNS topic `opennova-server-alerts` (`<name>-alerts`), which emails
  `alerts/email` (confirm the subscription from the first email AWS sends): the server's
  heartbeat missing for 5 minutes, more than 3 boots in 15 minutes, a failed hosted
  server, a lost machine, 3 failed launches in 15 minutes, a billing sync older than an
  hour, 5 refused webhooks in 15 minutes, the entitlement stops held for 5 minutes
  (`limits_held`: the supervisor's mass-stop guard keeps over-entitled servers running
  until billing is trusted again; the admin billing view says why), the service
  instance's status checks (a failed system check recovers the instance, a failed
  instance check reboots it), its CPU over 80% for 15 minutes, the database's CPU
  credits and free storage, and the Route 53 health check on `http://<Elastic
  IP>:8080/healthz`. The heartbeat is the server's JSON line every 60 s; its fields are
  metrics in `OpenNova/<deployment>`.
- **Budgets** (prod's, counting every OpenNova deployment): the month's cost
  (`budget_monthly_usd`) and the EC2 data transfer out (`budget_egress_gb`): an email at
  80% and at a 100% forecast; Cost Anomaly Detection emails daily anomalies over
  `cost_anomaly_threshold_usd`. The hosted servers' egress is metered and shown, never
  capped. The account may hold other projects, so both budgets count only what carries
  the `opennova:deployment` tag (every resource the stack makes, and the machines the
  service launches). Cost Explorer sees a tag once it is an active cost allocation tag:
  a day after the first apply, activate it once (until then the budgets read zero):

  ```bash
  ./deploy/run.sh aws ce update-cost-allocation-tags-status \
    --cost-allocation-tags-status TagKey=opennova:deployment,Status=Active
  ```
- **A shell.** `./deploy/run.sh app shell` (Session Manager; the toolbox carries the
  plugin). `./deploy/run.sh db psql` opens psql as the service's role there; `db psql -c
  'SELECT ...'` runs one statement.
- **Replacing the service instance.** It keeps no state: `./deploy/run.sh infra apply
  --allow-destroy -replace=aws_instance.service` makes a new one, which takes the network
  interface and the Elastic IP and converges to the current release at its first boot
  (5 minutes or so; the service is down meanwhile, the hosted servers keep running on
  their machines and list again). An OS upgrade is the same replace (the instance
  ignores newer images otherwise). A changed first-boot script
  (`deploy/host/service-user-data.sh`) or converge unit (`opennova-converge.service`),
  a comment included, is the instance's user data, so it shows in the plan as a
  replacement, and `infra apply` refuses that plan until you take the replacement on
  purpose, at a quiet time, with `--allow-destroy`.
- **Changing a secret.** Edit the vault, `secrets sync`, `app converge`. A new
  `database_password` takes `db bootstrap` (it sets the role's password) between the sync
  and the converge.

### Cost

About $90 a month for the service tier before the machines:

| | a month |
|---|---|
| the service instance, `c7i.large` on demand | $65 |
| its Elastic IP and 30 GiB root volume | $6 |
| the database, `db.t4g.micro`, 20 GiB gp3, backups | $14 |
| CloudWatch logs, metrics, alarms; the health check | $4 |
| S3 (releases, dumps), SSM | under $1 |

The machines: a full shared machine (`t3a.large`, its address and volumes, about $60) is
about $5 a server at 12 servers, before egress; an own machine (`c7a.medium`) about $43.
Egress is about $0.09 a GB, and a full 64-player server sends about 1.8 GB an hour, so
egress can cost more than the machine: the egress budget is the alarm. The first shared
machine runs at a loss until it fills.

## Moving from the single-host stack

Once, to move a deployment made before this layout (one `c7i.large` driven over SSH,
SQLite on its disk, the Elastic IP `aws_eip.server[0]`) onto it. The database starts
empty: announce the reset (accounts, CD keys and DFX2 activations go; a client holding an
old CDKIID re-mints). Rehearse it on a staging deployment first (with Stripe test mode).

**Before the window**

1. From the old checkout, take a last SQLite backup (`./deploy/run.sh backup now`).
2. On the new checkout: `secrets init`; fill `stripe-test` (and `stripe-live` when ready)
   and `alerts/email`; `secrets check`.
3. `mv infra/aws/terraform.tfvars infra/aws/prod.tfvars`, and set in it: the Elastic IP's
   allocation id (`./deploy/run.sh aws ec2 describe-addresses --public-ips <EIP> --query
   'Addresses[0].AllocationId'`) as `service_eip_allocation_id`; `azs`, the old subnet's
   zone first (`./deploy/run.sh aws ec2 describe-subnets --filters
   Name=cidr-block,Values=10.44.0.0/24`); and, should the old instance hold the private
   address `10.44.0.10`, another `service_private_ip_host`. Drop the keys the new
   variables no longer have (`ssh_allowed_cidr`, `allocate_eip`, `backup_bucket_name`:
   its value is `ops_bucket_name` now).
4. Request the vCPU quota (section 6) and make the data snapshots again if hosting is on
   (every machine mounts every data set by its label).
5. Put the deploy user on its least-privilege policies and make the role boundary every
   role here carries (`infra/aws/iam/README.md`, "Applying"); an apply that creates a role
   fails without the boundary policy. Run `./deploy/run.sh infra list` first: a downloads
   certificate still in the state (under the retired `us_east_1` alias) needs an
   `infra state-rm` before the new code plans.

**The window**

1. Maintenance on (the admin "NovaWorld service" page), and **stop all** hosted servers;
   wait until `./deploy/run.sh aws ec2 describe-instances --filters
   Name=tag:opennova:hosted,Values=1 Name=instance-state-name,Values=running` lists none.
2. An image of the old host, for a rollback: `./deploy/run.sh aws ec2 create-image
   --instance-id <old> --name opennova-server-pre-cutover --no-reboot`; wait until it is
   `available`.
3. If `./deploy/run.sh infra list` names a resource of the retired `us_east_1` provider
   (the old downloads certificate), `infra state-rm` it and delete the certificate by hand
   (ACM, us-east-1).
4. `./deploy/run.sh infra state-rm 'aws_eip.server[0]'` (the configuration's `removed`
   block forgets it as well), then check it still exists:
   `./deploy/run.sh aws ec2 describe-addresses --allocation-ids <id>`.
5. `./deploy/run.sh infra plan`, and read it: the network resources move (no subnet,
   VPC or route table is destroyed); the old instance, its security group, key pair and
   backup role go; the service instance, its interface, the database and the rest are
   added; the Elastic IP is not destroyed (an `aws_eip_association` takes it); the DNS
   records' content is unchanged. Then `infra apply --allow-destroy` (the old instance
   is an instance it destroys: check that the list it prints before asking names nothing
   else, then type `yes`; the database takes 10 to 15 minutes; the old instance goes,
   and the address moves to the new interface).
6. `secrets sync`, then `db bootstrap`.
7. `./deploy/run.sh app deploy --tag sha-<server> --serve-tag sha-<serve> --hostd-tag
   sha-<serve>`: the server's first boot applies every migration to the empty database.
8. Make the first site admin (section 3), then turn maintenance off.

**Rollback** (until the new stack is accepted): launch the image into public subnet a
(`./deploy/run.sh aws ec2 run-instances --image-id <the image> --instance-type c7i.large
--subnet-id <public a> --security-group-ids <a group admitting the old ports, 80
included> --key-name <a key pair, if you need the old toolbox's SSH>`), move the address
to it (`aws ec2 associate-address --allocation-id <id> --instance-id <it>
--allow-reassociation`), and set the zone's SSL/TLS mode back to "Flexible" (the old
nginx serves :80). The old SQLite database is on the image. The Elastic IP is never
released either way; moving it back to the new service instance is `infra apply
--allow-destroy` (it replaces the association).

### Smoke tests

- **A. Edge and ports.** `curl -I https://<domain>/` answers 200; `curl -m 5
  http://<EIP>/` times out (no port 80); `curl -i http://<EIP>:8080/api/health` answers
  403 (`proxy_required`); `curl -i http://<EIP>:8080/jop_2.gsb` answers 200; `curl -i
  http://<EIP>:8080/healthz` answers 200 `ok`; `curl -m 5 http://<EIP>:8070/` times out
  (the internal port); `curl https://<domain>/api/server-info` names the Elastic IP.
- **B. Operations.** `app status` shows every service healthy; `app logs` shows a stream
  per service; the `Heartbeat` metric has points; the SNS email subscription is
  confirmed; the Route 53 health check is green.
- **C. Accounts.** Register, log in, log out; the admin pages open.
- **D. Billing.** Stripe's "Send test webhook" answers 2xx; subscribe to Tier 1 ($5, test
  card in test mode) or give a comp grant; the account's entitlement shows it.
- **E. Retail.** A stock JO client signs in through the gate and lists the lobby.
- **F. A shared machine.** Start a server: a shared machine launches, the server lands on
  slot k and is joinable by a retail and an OpenNova client.
- **G. A second account** starts one: it lands on slot k+1 of the same machine.
- **H. A service restart** (`app restart novaworld`) keeps the hosted servers running and
  re-lists them; measure the gap.
- **I. Stop** a server: its slot frees; stop the last: the machine ends after the idle
  time.
- **J. Stop all** (the kill switch) stops everything and the machines end.
- **K. Alarms.** On staging: `app shell`, then `docker kill -s STOP <the novaworld
  container>`: the heartbeat alarm emails within 5 minutes, the health check goes red;
  `docker kill -s CONT` clears both.
