# Deploying your own OpenNova NovaWorld instance

This repository deploys a complete NovaWorld stack to AWS, behind Cloudflare: the gate,
the lobby, the legacy HTTP services, the web portal, the fleet program, the background
jobs and the data sets' jobs, a PostgreSQL database, a Valkey cache, and the fleet of machines the
hosted game servers run on. Anyone can stand up their own instance with the commands
the project maintainers use. The original games reach your server through a
hosts-file line that points `gs.novaworld.net` at its gate's Elastic IP (the OpenNova
Switcher writes it; DEVELOPING.md, "Test with retail Joint Operations"). The stack, like
the rest of OpenNova, is pre-1.0 and experimental.

- **Try it locally first.** `scripts/dev_stack.sh up` runs the same six programs, the
  portal, PostgreSQL, Valkey and hosted game servers on one machine under Docker, with no
  account of any kind (DEVELOPING.md, "Run the NovaWorld servers locally").
- **The whole bring-up, in order,** is the checklist "From zero, in order" below; the
  numbered sections it points into explain each step.
- **Under your own names** (your domain, your GitHub owner, your vault): "Running it
  under your own names" lists every place OpenNova's own names are written down.

The stack in short: the NovaWorld service is six programs, each its own image and an ECS
service on Fargate across two availability zones, whose tasks keep no state of their own:

| Program | Runs | Tasks (default) | Reached at |
|---|---|---|---|
| gate | the gate (UDP 7597; the hosts' status blobs land there too) | 1, 0.25 vCPU / 0.5 GiB | the public network load balancer, at the gate's Elastic IP |
| lobby | a lobby instance (UDP 64206) | 1, 0.25 vCPU / 0.5 GiB | the public network load balancer, at the gate's Elastic IP: one lobby address for every lobby task, which the gate hands out |
| web | the retail HTTP routes and the website's API (8080) and the portal's nginx (443, and 8082 in front of the retail routes) | 1 (up to its `max` on its CPU), 0.5 vCPU / 1 GiB | the load balancer: 443 from Cloudflare, 8080 from everyone (to the portal's 8082) |
| fleet | the fleet's reconcile pass, the admin links, the console drain, the machines' internal listener (8070); no job and no cloud call | 1 (elected; a standby may wait beside it), 0.25 vCPU / 0.5 GiB | `fleet.<deployment>.opennova.internal`, its Cloud Map name |
| worker | the background jobs and their schedules (the fleet's AWS calls, billing sync, mail, prunes) | 1 (up to its `max` on its CPU), 0.5 vCPU / 1 GiB | nothing inbound |
| data | the data sets' jobs over the game data's store (a tree's finalize, a release's validate and apply, the prune) | 1 (fixed: its jobs' caps are global), 0.25 vCPU / 1 GiB and 40 GiB of task storage | nothing inbound |

Its database is RDS PostgreSQL 17 in private subnets, on a small burstable class in
one zone while traffic is small; web's sessions and rate limits live in ElastiCache for Valkey beside it; its
secrets live in SSM Parameter Store, copied from a 1Password vault, and ECS hands each task
its own; a release is each program's task definition revision with its images pinned by
digest; its logs go to CloudWatch; the nightly database dump is a scheduled task. No SSH
anywhere: a shell is ECS Exec. The gate's Elastic IP is one no Terraform plan can release,
the load balancer's address in its first zone, where retail clients reach the gate, the
lobby and the retail HTTP routes; the website reaches web through Cloudflare alone, over
TLS Cloudflare verifies. Hosted game servers run on machines the service launches itself:
shared machines holding up to 6 servers, and own machines (a paid add-on) holding one.

## Deployments: dev, staging and prod

There are three places the service runs, and nothing of one reaches another by accident:

| | dev | staging | prod |
|---|---|---|---|
| What it is | a developer's local stack: the six programs, the portal, PostgreSQL and Valkey under docker compose (`scripts/dev_stack.sh`, `deploy/compose`, DEVELOPING.md) | the rehearsal: prod's shape, every change runs here first | the service players use |
| Where | the developer's machine | its own AWS account, `opennova-staging`, in the Organization | its own AWS account, `opennova-prod` |
| Site | `http://localhost:8000` | `https://staging.<domain>` | `https://<domain>` (and `www`) |
| Retail gate | the developer's LAN address | its own Elastic IP | the Elastic IP retail hosts files name (`gs.novaworld.net`), which prod adopts and never releases |
| Gate name (OpenNova's client, `opennova-serve --master-host`) | none (`127.0.0.1` in a source run, or `--novaworld-host`) | `nw.staging.<domain>` | `nw.<domain>` |
| AWS credentials | none | its deploy user's key (vault `aws-staging`), or its GitHub environment's OIDC role | its deploy user's key (vault `aws-prod`), or its GitHub environment's OIDC role |
| Stripe | test mode, the restricted key (`stripe-test/restricted_key`) | test mode (`stripe-test`) | live mode (`stripe-live`) |
| Mail | none (no AWS credentials: logged as not sent) | its own SES identity, `staging.<domain>` (off unless `mail_sender` is set) | its own SES identity, `<domain>`, `support@<domain>` |

**Separate, per deployment.** Everything AWS: each deployment is an account of its own
(the Organization's management account holds no OpenNova resource), so its VPC, cluster,
database, cache, buckets, SSM parameters, logs, alarms and their topic, budgets, SES
identities and sandbox, Cloud Map namespace, hosted machines and their game data (its
bucket's `data/`), and its Terraform state (a versioned, encrypted bucket in that account, locked by S3's own lock
file) are its own, and no IAM principal of one account can touch the other's. Each
deployment's credentials: its deploy user (`opennova-deploy` in its account; its key in
its own vault item) and its GitHub environment's OIDC role (which trusts that
environment's jobs alone). A command holds its own deployment's key alone, with two
exceptions, each one way and named by its flag: `backup import --from-deployment prod`
on staging reads prod's dump with prod's key, and `app deploy --promote staging` on prod
reads staging's release with staging's key; each in a subshell of its own, the two keys
never in one call. Its vault items
(`aws-<deployment>`, `app-<deployment>`, `rds-<deployment>`), its Stripe mode, its site's hostname, its origin certificate (staging's names
`staging.<domain>` alone, so it can never serve prod's), its edge authentication secret
and rule, its database's dumps (`backup restore` takes the deployment's own alone).

**Shared on purpose.**

- The Organization and its management account: billing, and the one user that runs the
  `org` stack (`opennova-org`, which may read the Organization, make accounts and enter
  each member account's `OrganizationAccountAccessRole`, and nothing else).
- The 1Password vault, with separate items per deployment (above) and a few shared ones:
  `aws-org`, `cloudflare`, `alerts`, `email`, `stripe-test`, `stripe-live`,
  `github-terraform`, `reference-assets`.
- The Cloudflare zone: one zone holds every deployment's records. Its zone-wide settings,
  its rulesets (each deployment's edge authentication rule with that deployment's own
  secret, the Stripe webhook's rule) and Email Routing are one stack, `infra/zone`; each
  deployment's own records are its own stack's.
- The container images on GHCR: built once per commit, deployed to staging by tag, and
  promoted to prod by digest (`app deploy --promote staging`): prod never runs an image
  built again.
- The GitHub repository and its CI; its two deploy environments, `staging` and `prod`
  (`infra/github`).
- Stripe's test mode: staging, the local stack and CI. Each database stamps its own
  instance id on the customers it makes and ignores every other, and the local stack and
  CI hold a restricted key with the server's permissions alone, never staging's secret key
  or its webhook's signing secret.
- Where the alarms, budgets and support mail go (`alerts/email`, `email/forward_to`).

**How a change flows.** Local work on the dev stack; a pull request (CI builds and tests,
and its merge publishes the images, tagged `sha-<commit>`); staging: `./deploy/run.sh
--deployment staging infra apply` for an infrastructure change, `--deployment staging app
deploy --tag sha-<commit>` for the programs, then the rehearsal (the smoke tests); prod:
`--deployment prod infra apply` with the same commit's Terraform, and `--deployment prod
app deploy --promote staging`, which pins exactly the images staging's current release
runs, by digest (or the deploy workflow's `prod` run, which does the same after its
reviewers approve).

**Guard rails.**

- A toolbox command that changes a deployment acts on the one `--deployment` names:
  there is no default and no environment variable for it. A read-only command (`app
  status`, `app logs`, `secrets check`, `infra output`, ...) without it reads prod and
  says so. Every command prints the deployment it acts on (prod in red) and the account
  its credentials act in, and stops before any call when the credentials are another
  account's.
- Every command that changes prod asks first: type `prod` (or pass `--yes`; with no
  terminal and no `--yes` it refuses). `infra destroy` refuses prod outright, and asks for
  `staging` on staging.
- Data and images cross one way alone: prod's dump reaches staging only through `backup
  import --from-deployment prod` (the restore drill), nothing of staging's database reaches
  prod; staging's images reach prod only by `--promote staging`. These two are the only
  commands that hold the other deployment's key, for that one read.
- Prod's GitHub environment requires its reviewers' approval and deploys from protected
  branches alone; its role trusts that environment alone.
- `deploy/bin/tests/deployment_guard_test.sh` (CI's infra workflow) checks these rules of
  the toolbox offline, and `infra/aws/tests` the stack's separation for both deployments.

## What you need

- **Docker** on the machine you deploy from. The deploy toolbox (1Password CLI,
  Terraform, the AWS CLI, the docker client) runs in a container.
- **On that machine, for the few [admin] steps** (section 3: the Organization, its
  accounts and the `opennova-org` user, done once outside the toolbox): the AWS CLI
  with the management account's administrator, the 1Password CLI `op` signed in as you,
  and `jq`.
- **A 1Password account** (any paid plan has service accounts): its vault holds every
  secret and every identifier a rebuild needs.
- **An AWS management account**, whose Organization holds three member accounts, one per
  deployment (section 3) and one for the CI runners (section "CI runners").
- **A RunsOn license** for the CI runners (section "CI runners"), or the standard runners
  alone ("Running it under your own names").
- **A domain on Cloudflare** (the Free plan does): add the domain to Cloudflare and set
  the registrar's nameservers to the two Cloudflare gives it. Cloudflare holds every
  record, proxies the website (the origin admits Cloudflare's address ranges alone and
  presents a Cloudflare Origin CA certificate) and forwards `support@<domain>` and the
  member accounts' root addresses (Email Routing).
- **A mailbox of yours** for that forwarding (`email/forward_to`), and an address for
  the alarms and budgets (`alerts/email`).
- **A GitHub repository** (your fork): CI builds the images and publishes them to its
  owner's container registry (GHCR), and its deploy workflow deploys from two
  environments (section 5, "The deploy workflow").
- **A Stripe account** for the monthly support tiers (the paid hosting). Without one the
  stack runs with billing off: nobody can subscribe, and only an admin's comp grant lets
  an account host servers.

No secret is ever committed to the repository or written to your disk outside a
container's in-memory tmpfs, except what the Terraform states hold (each deployment's
deploy user key in the org stack's, the origin certificate's key in each deployment's;
they live in versioned, encrypted buckets the deploy users alone reach). The only secret
you handle directly is a 1Password service-account token.

## From zero, in order

The whole bring-up, an empty management account to a running staging and prod. Each step
points at the section that explains it. **[admin]** steps run on your machine as the
management account's administrator; the rest are the toolbox, `./deploy/run.sh`
(`.\deploy\run.ps1` on Windows), with `OP_SERVICE_ACCOUNT_TOKEN` exported from step 2 on.

0. **Your own names**, for a deployment of your own: the edits in "Running it under your
   own names" (below), on your fork's `master`.
1. **The vault** (section 1): `op vault create OpenNova-Deploy` and the `cloudflare` item
   (`api_token`, `zone_id`, `domain`); Bot Fight Mode off.
2. **The service account and the generated secrets** (section 2): `op service-account
   create ...`, `export OP_SERVICE_ACCOUNT_TOKEN=ops_...`, then `./deploy/run.sh
   --deployment staging secrets init` and `./deploy/run.sh --deployment prod secrets
   init`, both now: the org stack reads both deployments' items, and the zone's both edge
   secrets. Fill `alerts/email` and `email/forward_to` in the 1Password app.
3. **[admin] The Organization and its accounts** (section 3, steps 1 and 2): the
   Organization's and the management account's ids into `aws-org`; IAM Identity Center
   enabled in the console, and your user's `sso_given_name`, `sso_family_name` and
   `sso_email` into `aws-org`; `aws organizations create-account` for `opennova-prod`,
   `opennova-staging` and `opennova-ci`, each one's id, name and root address into
   `aws-<deployment>` (`aws-ci` for the last).
4. **[admin] The `opennova-org` user** and its key into `aws-org` (section 3, step 3;
   `infra/aws/iam/README.md`).
5. **The org stack** (section 3, step 4): `./deploy/run.sh org plan`, then `org apply`.
   It writes each deployment's `state_bucket`, deploy key and `github_deploy_role_arn`
   into the vault; from here `./deploy/run.sh --deployment staging secrets check`
   resolves every reference (before it, the deploy keys it checks do not exist yet).
6. **The shared stacks** (section 3, step 5): `./deploy/run.sh zone apply`, then click
   Cloudflare's verification link (section 4, "Email"), then [admin] give each member
   account's root user a password and an MFA device. Fill the `github-terraform` and
   `reference-assets` items and `./deploy/run.sh github apply` (section "GitHub"), or on
   a GitHub organization of your own make the two environments by hand ("Running it
   under your own names"). Then the CI runners: the `runs-on` item, `./deploy/run.sh ci
   apply`, the GitHub App and the fork approval setting (section "CI runners").
7. **[admin] Per account, in its console** (section 3, step 6): SES production access,
   the Fargate and EC2 quotas, Cost Explorer once; in `opennova-ci`, its EC2 quotas alone
   (section "CI runners").
8. **The images** (section 5): a push to your repository's `master` that touches their
   paths publishes them (`.github/workflows/novaworld-images.yml`, `serve-image.yml`; a
   manual run builds them but never pushes); then make each GHCR package public. They
   come before step 9: `db bootstrap` already runs the backup image.
9. **Staging's infrastructure** (section 4), with `D='--deployment staging'`:
   `./deploy/run.sh $D secrets sync`, `infra plan`, `infra apply`, `db bootstrap`. Confirm
   the alarm topic's subscription from the first email AWS sends ("Operating").
10. **Stripe's test mode** ("Monthly support tiers (Stripe)"): `stripe-test/secret_key`
    and `restricted_key` by hand, `./deploy/run.sh $D stripe setup`, `stripe check`, the
    dashboard's settings, then `secrets sync`.
11. **Staging's first release**, from the toolbox, never the workflow (section 5):
    `./deploy/run.sh $D app deploy --tag sha-<programs> --serve-tag sha-<serve>
    --hostd-tag sha-<serve>` (both images come from one `serve-image.yml` run, so one
    tag); the first site admin (section 4, "The first site admin"); the game data on the
    site's Game data page (section 7, "Turning it on"); Stripe's "Send test webhook";
    the smoke tests ("Smoke tests").
12. **Prod**, with `P='--deployment prod'`: its gate's Elastic IP, made once in the prod
    account (`./deploy/run.sh $P aws ec2 allocate-address --domain vpc
    --tag-specifications 'ResourceType=elastic-ip,Tags=[{Key=opennova:deployment,Value=opennova-prod}]'`),
    its `AllocationId` into `aws-prod/gate_eip_allocation_id` (section 4, "The
    var-file"); `secrets sync`, `infra apply`, `db bootstrap`; Stripe's live mode (the
    site's legal pages written first, then the activation, `stripe setup`, `secrets
    sync`); `app deploy --promote staging`, the first site admin, the game data, the
    smoke tests. Later releases may run from the deploy workflow.

## Running it under your own names

The stack is OpenNova's own deployment, so a few files name OpenNova's domain, GitHub
owner, vault and regions. Everything else comes from the vault. For a deployment of your
own, change these on your fork:

| Where | What it names | For your own |
|---|---|---|
| `OP_VAULT` | the vault, `OpenNova-Deploy` by default | `export OP_VAULT=<name>` before `./deploy/run.sh`; the commands in sections 1 and 2, `infra/aws/iam/README.md` and `deploy/env/billing.*.env.tpl` write the default out |
| `cloudflare/domain` (the vault) | your domain | the hosts follow from it: `<domain>` and `www`, `staging.<domain>`, `nw.<domain>` and `nw.staging.<domain>` (the gates), `update.<domain>` and `update.staging.<domain>` (the update service) |
| `infra/aws/prod.tfvars`, `staging.tfvars` | `image_registry` (default `ghcr.io/opennova-net`), `azs` (`us-east-1a`, `us-east-1b`) | `image_registry = "ghcr.io/<your owner>"` in both. Another region is the `region` fields of `aws-org` and `aws-<deployment>` (`secrets init` writes `us-east-1`) and `azs`; the Route 53 health check's alarm exists in `us-east-1` alone |
| `infra/org/variables.tf` | `github_repository`, `opennova-net/opennova`: the repository whose deploy workflow may assume each account's deploy role | its default (the toolbox passes none) |
| `infra/ci/variables.tf` | `github_organization`, `opennova-net`: the organization whose GitHub App the RunsOn setup page registers | its default |
| `infra/github/` | the GitHub organization `opennova-net`, all of it | adapt it to your organization, or leave it out and make what the deploy workflow needs by hand: the repository environments `staging` and `prod` (prod with required reviewers), each with the variables `AWS_DEPLOY_ROLE_ARN` (the vault's `aws-<deployment>/github_deploy_role_arn`), `AWS_REGION` and `AWS_ACCOUNT_ID`; optionally the secret `STRIPE_TEST_SECRET_KEY` (the billing end-to-end test) |
| `.github/workflows/ci.yml` | RunsOn's runner labels on the build and test legs (section "CI runners"), and the retail legs' private reference repositories (`REFERENCE_ASSETS_TOKEN`) | your own RunsOn stack, or `windows-latest` and `ubuntu-latest` in place of the labels; no retail legs. The image workflows run on `ubuntu-latest` and need neither |
| `infra/zone/variables.tf` | `email_routes`: `support`, `aws-prod`, `aws-staging`, `aws-ci` | its default, if your accounts' root addresses differ (the zone stack takes no var-file) |
| `web/src/utils/links.ts`, `web/index.html` | the site's links to OpenNova's repository and gate (`nw.opennova.net`), its `og:` tags | your own |
| `web/src/pages/legal/*.vue` | the terms, privacy and refunds pages: drafts with placeholders and a draft banner | your text, before Stripe's live mode (its activation needs them) |
| the game and the Switcher | OpenNova's NovaWorld (`nw.opennova.net`, `https://opennova.net`) | players reach yours with `opennova.exe -- --novaworld-host nw.<domain>`, or the Switcher's **Custom address...** (your gate's address) for the original exes; a self-run `opennova-serve` lists with `--master-host nw.<domain>` |

## Rebuilding from the vault

Everything a rebuild needs that Terraform cannot make lives in the 1Password vault
(`OpenNova-Deploy`, or the one `OP_VAULT` names); the repository holds `op://`
references, never a value. A rebuild is: restore the vault, then "From zero, in order"
(above), skipping each manual step whose vault field is already filled. The vault's
contract, item by item (`deploy/env/*.tpl` holds every reference the toolbox reads):

| Item | Field | Filled by |
|---|---|---|
| `aws-org` | `account_id`, `organization_id`, `region` | hand: the management account, its Organization (section "The Organization and the accounts") |
| `aws-org` | `access_key_id`, `secret_access_key` | hand: the `opennova-org` user's key (`infra/aws/iam/README.md`'s commands write it) |
| `aws-org` | `sso_given_name`, `sso_family_name`, `sso_email` | hand: the maintainer's IAM Identity Center user |
| `aws-org` | `sso_portal_url` | `./deploy/run.sh org apply` |
| `aws-prod`, `aws-staging` | `account_id`, `account_name`, `root_email`, `region` | hand: the member account (`secrets init` makes the placeholders) |
| `aws-prod`, `aws-staging` | `state_bucket`, `access_key_id`, `secret_access_key`, `github_deploy_role_arn` | `./deploy/run.sh org apply` |
| `aws-prod` | `gate_eip_allocation_id` | hand: the gate's Elastic IP in the prod account, allocated once (section 4, "The var-file") |
| `aws-ci` | `account_id`, `account_name`, `root_email`, `region` | hand: the CI runners' account (`secrets init` makes the placeholders; section "CI runners") |
| `runs-on` | `key` | hand: RunsOn's license key (section "CI runners") |
| `cloudflare` | `api_token`, `zone_id`, `domain` | hand (section 1) |
| `app-prod`, `app-staging` | `admin_api_token`, `database_password`, `redis_password`, `edge_auth_secret` | `secrets init` |
| `rds-prod`, `rds-staging` | `master_password` | `secrets init` |
| `stripe-test`, `stripe-live` | `secret_key` (and `restricted_key` in `stripe-test`) | hand (Stripe's dashboard) |
| `stripe-test`, `stripe-live` | `webhook_secret`, `price_*` | `stripe setup` |
| `alerts`, `email` | `email`, `forward_to` | hand |
| `github-terraform` | `token`, `billing_email`, `members` | hand (section "GitHub") |
| `reference-assets` | `token` | hand (section "GitHub") |

The manual steps outside the vault: the Organization and its member accounts, the
`opennova-org` user's policy, the CI runners' GitHub App, EC2 quotas and fork approval
setting (section "CI runners"), each account's SES production access and service quotas
(below), the root addresses' Email Routing verification click, the GHCR packages' public
visibility and the Stripe dashboard's settings.

## 1. Create the vault

Create a vault named `OpenNova-Deploy`; the service account comes in step 2 (it needs read
and write access: the write for the generated secrets and what the org stack writes
back). Create the item you supply yourself:

```bash
op vault create OpenNova-Deploy

op item create --vault OpenNova-Deploy --title cloudflare --category 'API Credential' \
  api_token=... zone_id=... domain=example.com
```

The Cloudflare token (`cloudflare/api_token`: My Profile, API Tokens, Create Token,
Custom token) needs exactly these, the zone ones for the one zone and the account one
for its account:

| Scope | Permission | What needs it (`infra/aws/edge.tf`, `mail.tf`, `infra/zone`) |
|---|---|---|
| Zone | DNS / Edit | each deployment's site records, its outbound mail's DKIM and MAIL FROM records |
| Zone | Zone Settings / Edit | SSL/TLS "Full (strict)", Always Use HTTPS, the minimum TLS version; turning Email Routing on |
| Zone | SSL and Certificates / Edit | each deployment's origin certificate (Origin CA) |
| Zone | Zone WAF / Edit | the Stripe webhook's skip rule (custom rules) |
| Zone | Transform Rules / Edit | edge authentication's header |
| Zone | Email Routing Rules / Edit | `support@<domain>`'s rules, and the member accounts' root addresses' |
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

Everything else is made by the toolbox once the token works (step 2): `./deploy/run.sh
--deployment <prod|staging> secrets init` adds what the vault lacks, for that deployment
and the shared items, and never overwrites a field. It prints item and field names, never
a value:

| Item | Fields | What `secrets init` puts there |
|---|---|---|
| `app-<deployment>` | `admin_api_token`, `database_password`, `redis_password`, `edge_auth_secret` | random values |
| `rds-<deployment>` | `master_password` | random |
| `aws-<deployment>` | `account_id`, `root_email` | `REPLACE_ME` |
| `aws-<deployment>` | `account_name`, `region` | `opennova-<deployment>`, `us-east-1` |
| `aws-org` | `account_id`, `organization_id` | `REPLACE_ME` |
| `aws-org` | `region` | `us-east-1` |
| `stripe-test`, `stripe-live` | `secret_key`, `webhook_secret`, `price_tier1`, `price_tier2`, `price_tier3`, `price_own_machine`, `price_static_address` | `REPLACE_ME` |
| `stripe-test` | `restricted_key` (the local stack's and CI's key: "GitHub" below) | `REPLACE_ME` |
| `alerts` | `email` | `REPLACE_ME` |
| `email` | `forward_to` | `REPLACE_ME` |

Fill the `REPLACE_ME` fields in the 1Password app (that keeps the keys out of shell
history): `alerts/email` is where every alarm, budget and cost anomaly writes;
`email/forward_to` is the mailbox `support@<domain>` and the member accounts' root
addresses forward to ("Email", section 4), and each account's SES sandbox recipient
("Outbound mail (SES)"); the `aws-*` fields, section "The Organization and the accounts";
in each Stripe item you fill `secret_key` (the Stripe dashboard) and `./deploy/run.sh
--deployment <prod|staging> stripe setup` the rest ("Monthly support tiers (Stripe)",
below). Optional fields the toolbox picks up when present:
`app-<deployment>/bootstrap_admin` (section 4) and
`app-<deployment>/edge_auth_secret_previous` (a rotation). There is no SSH item, and nothing
of the hosted servers' game data is in the vault (section 7: the site's Game data page holds
it, in the deployment's operations bucket).

The `op://` references the toolbox reads are listed in `deploy/env/`: `terraform.env.tpl`
(a deployment stack's), `org.env.tpl`, `zone.env.tpl` and `github.env.tpl` (the shared
stacks'), `aws.env.tpl` (whose access key a command acts with) and `runtime.env.tpl` (the
service's runtime secrets).

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
export OP_SERVICE_ACCOUNT_TOKEN=ops_...                         # the only secret you handle
./deploy/run.sh --deployment staging secrets init               # staging's items and the shared ones
./deploy/run.sh --deployment prod secrets init                  # prod's
```

`./deploy/run.sh --deployment staging secrets check`, once `org apply` (section 3) has
written the deploy keys it resolves too, resolves every reference without printing a
value, and reports each Stripe item as `OK (test mode, 3 tiers + own machine + static address)`, `placeholders: <fields> (billing
off)`, `absent (billing off)` or `FAILED: <field> (malformed)` / `(a test key in a live
item)`; a failed one makes it exit non-zero. The forms it checks: `secret_key`
`sk_<mode>_...` or `rk_<mode>_...` (the item's mode: `test` for `stripe-test`, `live` for
`stripe-live`), `webhook_secret` `whsec_...`, each price `price_...` (a comma list: the
first is sold, the rest are grandfathered), and `stripe-test/restricted_key` a test-mode
restricted key (`rk_test_...`). It also says whether `alerts/email` and `email/forward_to`
are still placeholders (an absent `email` item fails: every Terraform run reads it).

`./deploy/run.sh` (or `.\deploy\run.ps1`) rebuilds the toolbox image when
`deploy/Dockerfile` changed and runs it with the repository mounted. The toolbox is a
Debian image with pinned, checksummed releases of every tool.

A service account's requests are rate-limited: per hour per token, and per day for the
whole 1Password account (1Password's "Service account rate limits": on a Teams account,
1,000 reads an hour and 5,000 requests a day). So a run checks the token once (`op
whoami`), lists the vault once (3 reads), reads each item it needs once by its ids (1
read each) into a cache in the container's tmpfs, and resolves every `deploy/env/`
reference from that cache itself (no `op run`, `op inject` or `op read`). A command that
writes the vault (`secrets init`, `stripe setup`, `org apply`) reads the item fresh before
each write. A read that meets a rate limit or a server error is tried once more; one that
still fails stops the command, naming the item. What a command costs in read requests,
besides its one `op whoami` (`deploy/bin/tests/vault_requests_test.sh` counts them):

| Command | Read requests |
|---|---|
| `app`, `fleet`, `backup` and `aws` commands | 4 (5 for `app deploy --promote staging` and `backup import --from-deployment prod`, which read the other deployment's item) |
| `db bootstrap`, `db reset-db` | 5 |
| `secrets sync` | 6 |
| `secrets check` | 12 |
| `stripe check` | 5 |
| `infra plan`, `infra apply` | 9 |
| `org plan` | 6 |
| `org apply` | 9, and its 3 writes |
| `zone plan`, `github plan` | 8 |

`op service-account ratelimit` (with the token in `OP_SERVICE_ACCOUNT_TOKEN`) shows what
is left of the hour and the day.

## 3. The Organization and the accounts

Once, in this order. Each step names who runs it and the vault field it fills:
**[admin]** is the maintainer with the management account's administrator (the root user
or an admin role), outside the toolbox; everything else is the toolbox.

1. **[admin] The Organization.** `aws organizations describe-organization` (or `aws
   organizations create-organization --feature-set ALL` if there is none): its id
   (`o-...`) goes in `aws-org/organization_id`, the management account's id in
   `aws-org/account_id`. IAM Identity Center is enabled in the management account (the
   console, once); the org stack gives the maintainer's user its permission sets (step 4:
   `aws-org/sso_given_name`, `sso_family_name`, `sso_email`).
2. **[admin] The member accounts**, one per deployment, each with a root address at the
   domain that Email Routing forwards (`aws-prod@<domain>`, `aws-staging@<domain>`;
   `infra/zone` makes their rules, step 5), and the CI runners' (`aws-ci@<domain>`;
   section "CI runners"):

   ```bash
   aws organizations create-account --email aws-prod@<domain> --account-name opennova-prod
   aws organizations create-account --email aws-staging@<domain> --account-name opennova-staging
   aws organizations create-account --email aws-ci@<domain> --account-name opennova-ci
   aws organizations list-create-account-status --states SUCCEEDED   # each new account's id
   ```

   Each account's id goes in `aws-<deployment>/account_id` (`aws-ci/account_id` for
   `opennova-ci`), its name in `account_name`, its root address in `root_email`
   (`./deploy/run.sh --deployment <deployment> secrets init` makes the fields). Each account has the role `OrganizationAccountAccessRole`,
   which the management account may assume. Its root user gets a password once its
   address receives mail (step 5).
3. **[admin] The `opennova-org` user**, the management account's one OpenNova principal:
   `infra/aws/iam/opennova-org.json` filled with the five ids (the Organizations reads,
   account creation and tagging, Identity Center's user and assignments, and assuming the
   three member accounts' `OrganizationAccountAccessRole`, nothing else). The commands
   (`infra/aws/iam/README.md`) make the user and put its key straight into
   `aws-org/access_key_id` and `secret_access_key`.
4. **The org stack:**

   ```bash
   ./deploy/run.sh org plan
   ./deploy/run.sh org apply                    # type org
   ```

   `infra/org` adopts the Organization and the three accounts, and in each deployment's
   account through its `OrganizationAccountAccessRole` makes: the Terraform state bucket
   (`opennova-tfstate-<account id>`, versioned, encrypted, TLS alone, reached by the
   account's deploy user and admin role alone), the account's S3 public access block, the
   two boundaries every role a deployment's stack makes must carry one of (the machine
   boundary on the machines' instance role and hostd's task role, the role boundary on
   every other role: `infra/aws/iam/opennova-machine-boundary.json` and
   `opennova-role-boundary.json`, each under IAM's 6,144 characters), the deploy user
   `opennova-deploy` with the deploy policies (`infra/aws/iam/opennova-deploy-*.json`,
   filled for the account; it writes a role under either boundary alone) and its access
   key, and the GitHub deploy role (trusting the repository's environment of the same name
   alone, under the role boundary; it rolls the programs and hostd, and execs into its
   deployment's web container to confirm hostd on the machines); in the management account, the
   Identity Center user and its two permission sets (`OpenNovaAdmin`, `OpenNovaReadOnly`)
   in every member account (`opennova-ci` gets nothing else here: section "CI runners").
   Its first run applies with a local state, then moves the state into prod's state
   bucket (`org/terraform.tfstate`), where it lives. Every apply writes back
   to the vault, keys never on the screen: each deployment's `state_bucket`,
   `access_key_id`, `secret_access_key` and `github_deploy_role_arn`, and
   `aws-org/sso_portal_url`, the access portal. Rotating a deploy key: `./deploy/run.sh
   org apply -replace='module.<deployment>.aws_iam_access_key.deploy'` (the vault gets the
   new key).

   The stack makes the Identity Center user (`aws_identitystore_user.maintainer`; its user
   name is `aws-org/sso_email`). Where a user of that name exists already (made in the
   console, or by an earlier stack), the apply stops at a `ConflictException`: adopt it
   instead, then apply again:

   ```bash
   # [admin]: the identity store and the user's id
   aws sso-admin list-instances --query 'Instances[0].IdentityStoreId' --output text
   aws identitystore get-user-id --identity-store-id <store id> \
     --alternate-identifier '{"UniqueAttribute":{"AttributePath":"userName","AttributeValue":"<sso_email>"}}'
   ./deploy/run.sh org import aws_identitystore_user.maintainer <store id>/<user id>
   ```
5. **The zone's and the organization's stacks** (their states in prod's state bucket):
   `./deploy/run.sh zone apply` (section 4, "The zone's shared settings": every
   deployment's edge authentication rule, Email Routing with the root addresses' rules)
   and `./deploy/run.sh github apply` (section "GitHub": the deploy environments and
   their variables). A zone holding rulesets made before this stack adopts them first
   (section 4, "The zone's shared settings").

   [admin] Then, the root addresses' routes live and Cloudflare's verification link
   clicked (section 4, "Email"): give each member account's root user a password (the
   sign-in page's "Forgot password" mails the root address) and an MFA device, then leave
   it unused.
6. **[admin] Per account, in its console** (the access portal, `aws-org/sso_portal_url`,
   with `OpenNovaAdmin`): Amazon SES, **Request production access** (prod at least;
   "Outbound mail (SES)"); Service Quotas: the Fargate On-Demand vCPU quota (L-3032A538:
   the defaults' tasks take about 4 vCPUs at once, a deploy's extra tasks and a one-off
   task included; a new account's quota can be lower, and more tasks take more) and the
   EC2 On-Demand Standard quota (L-1216C47A, 32 for the machines: section 7); Billing,
   Cost Explorer once (a member account's budgets read its own costs).

Then each deployment's infrastructure, staging first (section 4).

## 4. Stand up a deployment's infrastructure

Every command below names its deployment: staging first, then prod, the same commands
with `--deployment prod`.

### The var-file

The non-secret knobs live in `infra/aws/<deployment>.tfvars` (`prod.tfvars`,
`staging.tfvars`; `deployment.tfvars.example` names them all). It holds no secret and no
identifier of an account, so committing it keeps every operator on the same knobs. Never
name one `terraform.tfvars` or `*.auto.tfvars` (or their `.json` forms): Terraform loads
those into every run, so the toolbox refuses to run while one exists in `infra/aws/`.

The deployment's account, region and (prod) gate address come from the vault, as
`TF_VAR_deployment`, `TF_VAR_aws_account_id` (the AWS provider acts in that account alone),
`TF_VAR_region` and `TF_VAR_service_eip_allocation_id`.

Prod adopts the gate's Elastic IP, the one retail clients use: `aws-prod/gate_eip_allocation_id`
names it in the prod account. Make it once, before prod's first plan (the deploy user may
allocate an address that carries the deployment's tag):

```bash
./deploy/run.sh --deployment prod aws ec2 allocate-address --domain vpc \
  --tag-specifications 'ResourceType=elastic-ip,Tags=[{Key=opennova:deployment,Value=opennova-prod}]'
# its AllocationId goes in aws-prod/gate_eip_allocation_id (the 1Password app)
```

An address held in another account comes in by EC2's Elastic IP transfer instead (`aws ec2
enable-address-transfer` there, `accept-address-transfer` here), keeping its address, which
retail hosts files may already name. Terraform reads the address and tags it, and never
manages it: no plan can release it. It is the public load
balancer's address in the first zone (below, "The gate's address and the programs"). Prod
never makes an address of its own, so a prod plan without the allocation id stops at a
precondition. Staging gets a managed address of its own and never adopts one: its plan
stops too when an allocation id reaches it. Pin `azs` once the stack exists so a change
in the region's list never moves a subnet.

`image_registry` (default `ghcr.io/opennova-net`) names where the images are published
(section 5); a deployment of your own images sets it to `ghcr.io/<your owner>`.

### Plan and apply

```bash
./deploy/run.sh --deployment staging infra plan     # review the plan
./deploy/run.sh --deployment staging infra apply    # plans again, applies exactly that plan
```

`infra apply` saves its plan to a file and applies that file, never a plan made again.
It refuses a plan that destroys or replaces what no apply makes again or what the
service runs on: an Elastic IP or an association, an instance or a network interface,
the load balancer, the ECS cluster or a program's service, the fleet program's Cloud Map
namespace, the database, the cache, a bucket. Read the plan, then `infra apply
--allow-destroy` if that is meant. A plan that would make prod a new gate address is
refused whatever the flags. On prod it then prints what the plan does (`N to create, N
to update, N to replace...`) and applies the saved plan only once you type `prod`;
`--yes` answers for you, and a run with no terminal (a script) refuses without it. The
database also carries `prevent_destroy` and deletion protection. Further arguments pass
through (`infra apply -target=...`). Other `infra` commands: `state-rm ADDRESS` (forget
a resource, keep its object), `import ADDRESS ID`, `output`, `list`, and `destroy`
(refused for prod; on staging it asks for `staging`).

What it makes: the VPC (`10.44.0.0/16`: public subnets a/b for the tasks, the load
balancer and the machines, private a/b with no route out for the database and the
cache); the ECS cluster; per program a security group (the gate: its UDP port and its
health port from the load balancer alone; the lobby: its UDP port to everyone and its
health port from the load balancer; web: 443 and 8082 from the load balancer alone; the
fleet: the internal port from the machines alone; the worker and data: nothing; no 22, no
80 anywhere), a task role holding what
that program needs alone, a task definition (the template every deploy clones), an ECS
service on Fargate in both public subnets with a public IPv4 per task, made with no task
(nothing runs before a release names the program, below), and its task counts
(Application Auto Scaling between the program's `min` and `max`, web and the worker on
their CPU); the task execution role (the deployment's secrets and the log group); the
fleet program's Cloud Map name; the public network load balancer with the gate's Elastic IP (adopted on prod) in
zone a and one of its own in zone b, carrying the gate, the lobby and web, and its own
security group; the database and the cache (Valkey, one node by default); the backup task and its nightly schedule; the
SSM parameters the toolbox reads; the operations bucket (`opennova-<deployment>-ops-<account
id>`: the releases, the dumps and the game data's store, `data/`, with its lifecycle
rules); the logs, metrics and alarms; the account's budgets; the deployment's
Cloudflare records (its site's, proxied; the update service's and the gate's, unproxied), its origin
certificate; its outbound mail's SES identities and records (with a sender); the update
service's listeners; and (with `hosted_enabled`) the fleet cluster with its hostd daemon,
the fleet's launch templates, the machine role and web's and the data program's store
policies, and (with `fleet_max_addresses` above 0) the static addresses' Route 53 zone
`play.<domain>` with its Cloudflare NS records (section 7). A new
database takes 10 to 15 minutes, a new cache about as long. A service runs no task until
a release names its program (the release manifest's program list): a template's images
are unpinned (`image_tag`, `latest`), so no task starts from one, and its count's floor
is 0 and its heartbeat and running alarms off until then; the first `app deploy` naming
it starts it at its `desired` count and raises its floor to its `min`, which the next
`infra apply` keeps (section 5).

The state lives in the deployment's own account: `s3://<aws-<deployment>/state_bucket>/infra/aws/terraform.tfstate`,
versioned and encrypted, its lock S3's own lock file (`use_lockfile`), so two operators'
runs never overlap and every operator with the vault runs the same state; nothing is
pushed or pulled. The state holds the origin certificate's private key in plain text; the
bucket admits the account's deploy user and admin role alone. The database's master
password never enters the state (Terraform 1.11 write-only arguments): it reaches RDS at
creation, and again only when `db_master_password_version` in the var-file is bumped. The
cache's AUTH token does enter it (ElastiCache's provider has no write-only form of it).

### Secrets to the tasks

```bash
./deploy/run.sh --deployment staging secrets sync   # vault -> SSM SecureStrings
```

`secrets sync` reads each reference in `deploy/env/runtime.env.tpl` from the vault and
writes it to `/opennova/<deployment>/secret/<NAME>` in the deployment's account, each value
going vault, tmpfs file, `put-parameter --value file://...`: never an argument, never
printed. The Stripe fields are synced together, and only while the deployment's Stripe
item (`stripe-live` for prod, `stripe-test` for staging) is complete and well formed; with
placeholders billing stays off and none of them is synced. Fields marked optional
(`bootstrap_admin`, `edge_auth_secret_previous`) are synced while they exist; `secrets
sync --prune` deletes from SSM what the vault no longer names. ECS reads each task's
secrets as the task starts (its task definition names them, `valueFrom` the parameter),
and fails a task naming one SSM does not hold. So Terraform's task definitions name only
the secrets every deployment holds (`secrets init` makes them); the optional ones
(`ONNET_BOOTSTRAP_ADMIN`, `EDGE_AUTH_SECRET_PREVIOUS`, billing's set) are in each
template's `opennova.optional-secrets` label, and a deploy registers a revision naming the
ones SSM holds then. After a sync, `app converge` (or the next `app deploy`) brings every
program onto them. A required secret SSM lacks stops the deploy, naming it.

### The database

After the first apply, make the service's role and its database:

```bash
./deploy/run.sh --deployment staging db bootstrap
```

It runs as a one-off task of the backup task definition (`infra/aws/backup.tf`: the
backup image, with psql), as the RDS master user: it creates the role `novaworld` with
the `DATABASE_PASSWORD` secret as its password (so `secrets sync` first), and the
database `novaworld` owned by it, `TEMPLATE template0 ENCODING 'SQL_ASCII' LC_COLLATE
'C' LC_CTYPE 'C'` (the only kind the programs accept: retail hosts send cp1252 bytes,
which they store byte for byte). It is idempotent: run again, it only sets the role's
password again (how a changed `database_password` takes effect). The master password
reaches the task as a short-lived SecureString its role may read, which the command
deletes when it ends. TLS is required (`rds.force_ssl`), and every client verifies the
server (`sslmode=verify-full` against the pinned RDS CA bundle).

The instance is `db.t4g.micro` (`db_instance_class`: 2 vCPUs, 1 GiB), a burstable class
RDS runs in unlimited mode: a load past its baseline spends surplus CPU credits, billed
by the vCPU-hour, and is never throttled (the `db-cpu-credits` alarm says when its
balance runs low: a larger class such as `db.t4g.small`, or Performance Insights' top
statements). It runs in one zone, which RDS picks, while traffic is small: a zone's
loss or a patch takes it, and the service, down for minutes. Multi-AZ (`db_multi_az =
true`) adds a synchronous standby in the other private subnet's zone that RDS fails over
to on its own, the endpoint's name following it, in a minute or two.
`max_connections` is sized rather than left to RDS's memory formula (about 100 for
1 GiB): `db_pool_max` (7, the programs' `DATABASE_POOL_MAX`; at least 6, a worker's four
job threads, its keeper and its main loop) for each of `db_service_processes` (12)
program processes, plus 10 for the backup task, the operator's psql and the reserved
connections (94). The 12 are every program's tasks at its `max`, doubled for the new
tasks a deployment starts beside them: a task each (2 + 2 + 2 + 2 + 2 + 2); the plan refuses
a `max` past them (raise
`db_service_processes` with it; the `db-connections` alarm says when more connect).
A class change, a Multi-AZ change and `max_connections` (a static parameter) wait for
the weekly maintenance window (Sunday 09:00 UTC; `apply_immediately` is off). To take
them now, after the apply: `./deploy/run.sh --deployment <deployment> aws rds
modify-db-instance --db-instance-identifier <db_identifier> --apply-immediately` (a Multi-AZ conversion
runs with no outage; a class change on a Multi-AZ instance fails over, a minute or so,
and its reboot takes the new `max_connections`). A `max_connections` change alone (a
new `db_pool_max` or `db_service_processes`) waits for a reboot of its own:
`./deploy/run.sh --deployment <deployment> aws rds reboot-db-instance --db-instance-identifier <db_identifier>`
(a minute or so of database errors).

### The cache

`infra/aws/cache.tf` makes web's ephemeral store: ElastiCache for Valkey 8, one node
(`cache_node_count`, 1 while traffic is small: its loss drops the sessions, pins and rate
limits, and players log in again; 2 or more add replicas in the other zones that it fails
over to on its own; `cache_node_type`, default `cache.t4g.micro`); in-transit TLS required, encrypted at rest,
no snapshots, cluster mode off (one script writes a pin and its account's index
together), `maxmemory-policy volatile-lru` (every key has a TTL: a full node evicts the
key touched longest ago rather than fail a login; the evictions alarm says the node is
too small). It holds the retail login, join and host-key sessions, the pin cache and the
rate limiters, and nothing with an invariant (that stays in the database). Only web's
security group reaches it, on 6379. Web's task definition names it,
`ONNET_REDIS_URL=rediss://<primary endpoint>:6379`; the AUTH token is the vault's
`app-<deployment>/redis_password` (`secrets init` makes it), which Terraform hands to
ElastiCache (`TF_VAR_cache_auth_token`) and `secrets sync` hands to web
(`ONNET_REDIS_PASSWORD`). So run `secrets init` and `secrets sync` before the apply that
makes it.

**Rotating the token.** Set a new `redis_password` (`openssl rand -hex 32`), `infra
apply` (ElastiCache takes it beside the old one: `auth_token_update_strategy = ROTATE`),
`secrets sync`, `app restart web`, each with the deployment's `--deployment`. The old
token stays valid until the next rotation sets another.

**Permissions.** What the deploy user may do is `infra/aws/iam/opennova-deploy-*.json`,
rendered for each account by the org stack; a change to a document reaches both deploy
users at the next `./deploy/run.sh org apply` (`infra/aws/iam/README.md`).

### The gate's address and the programs

Retail clients find the gate by its Elastic IP alone. It is the public network load
balancer's address in zone a (its subnet mapping there; zone b has an address of its
own), cross-zone, so it reaches the tasks in either zone: the gate's UDP port (7597,
its target group checking each task's `/livez` on the health port, 8081), the lobby's
UDP port (64206, checked the same way) and web's retail HTTP port (8080, to the portal
nginx's retail server on 8082, which caps each request's body and hands it to web's
server with the client's address; checked on `/healthz` through it), all to IP targets
that keep each client's own address and port, and 443
(the portal's nginx, from Cloudflare's ranges alone; with proxied records only, since
without them there is no origin certificate and nginx listens on 80). Deletion
protection is on: the balancer holds the gate's address.
`ONNET_PUBLIC_HOST` (the gate's `POSTIPADDRESS`) and `ONNET_WEB_HOST` (the startup URL,
the web domain, `HOST_URL`, `GSB_SERVER`) both name that address, so no retail client
follows a DNS name; the site's records point at the balancer's DNS name. The gate's own
name, `nw.<domain>` (`nw.<deployment>.<domain>` off prod), is an unproxied A record at that
address: OpenNova's own client dials it for its OpenNova NovaWorld, and a self-run
`opennova-serve` lists on it with `--master-host`.

Every lobby task announces that address with the lobby's port (`ONNET_LOBBY_ENDPOINT`,
`<the gate's address>:64206`), each its own lobby instance behind it, and the gates hand
that one address out. The balancer keeps each client's flow (its address and port) on
one task, so a lobby sees each host's own address; a stock host reconnects only to the
lobby address the gate first gave it, so a host whose lobby task went away lands on a
live one at the same address (`apps/novaworld_common/README.md`, "Lobby instances"). A
lobby task leaving (a deploy, a restart) is deregistered first: the balancer sends it no
new flow, keeps its flows 10 s, then ends them, and ECS stops it only then. The
fleet program's tasks register in Cloud Map as `fleet.<deployment>.opennova.internal`
(an A record, TTL 10, private to the VPC); the leader alone binds the internal listener,
where the machines' hostd and `ONNET_INTERNAL_URL` reach it.

Every task answers `/livez` (its program's stall watchdog) on the health port (web on 8080
as well), which its container health check asks; ECS replaces a task that fails it, and a
deployment counts a new task only once it passes (and, behind the balancer, once the
balancer's check does). Web's `/healthz` also fails while the cache does not answer: the
balancer routes around it, and no task is replaced for it, since `/livez` is what ECS
asks.

Each program's sizes are a var-file setting (`gate`, `lobby`, `web`, `fleet`, `worker`,
`data`: a `cpu` and `memory` of a Fargate size, `min`, `max` and `desired`, any left out
keeping its default). `desired` is the count a new service starts with; Application
Auto Scaling holds each count between `min` and `max`, web's and the worker's on their
CPU (60% on average), the others where they start (data's three are one fixed count:
its jobs' caps are global, so more tasks would run none more at once). The programs' maxima, with a
deployment's extra tasks, must fit the database's `db_service_processes`; the plan says
when they do not. More lobbies are `lobby = { min = 2, max = 2, desired = 2 }`.

### Cloudflare: TLS to the origin

Each deployment's site records are proxied CNAMEs to its public load balancer (prod's `@`
and `www`, staging's `staging`); Cloudflare connects through it to a web task's nginx (the
portal container) on 443 (the balancer passes TCP through, so TLS runs end to end) and
verifies its certificate, a Cloudflare Origin CA certificate (ECDSA, 15 years) for the
deployment's own hostnames alone (prod's the domain and `www`, staging's
`staging.<domain>`), which its Terraform makes and hands the portal container through SSM
(`/opennova/<deployment>/tls/*`, which ECS puts in its environment as it starts); its
entrypoint writes it under `ORIGIN_TLS_DIR`. The zone is set to SSL/TLS "Full (strict)"
with "Always Use HTTPS" (`infra/zone`). Apply the zone setting together with an app
deploy whose nginx serves 443: in between, the site answers 52x. The balancer's 443
listener, its target group and its rules exist with proxied records alone
(`cloudflare_web_proxied`): without them there is no origin certificate, nginx listens on
80, and nothing reaches it but the retail port's `/api/` refusal. There is no port 80 on
the origin. The retail HTTP port (8080) has nothing to do with this: stock clients speak
plain HTTP to it at the gate's address (the gate's startup URL, `ONNET_WEB_HOST`), through
the portal nginx's retail server (8082, `web/nginx.conf`: a 16 KiB body cap, the client's
address in `X-Real-IP`), and `ONNET_API_PROXY_ONLY=1` refuses its `/api/` to anyone but
the portal's own HTTPS server, on the task's loopback (`ONNET_TRUSTED_PROXIES=127.0.0.1`).

Behind the proxied records every request reaches nginx from a Cloudflare edge, so nginx
takes the visitor's address from `CF-Connecting-IP`, for Cloudflare's published ranges
only (`web/cloudflare-realip.conf`; refresh it before a portal image release with `sh
web/cloudflare-realip.sh`, review and commit the diff). The load balancer's security
group takes the same ranges live from Cloudflare at each plan.

### The zone's shared settings (infra/zone)

One stack holds what the zone has once for every deployment: SSL/TLS "Full (strict)",
Always Use HTTPS and the minimum TLS version; the edge authentication rule of each
deployment (below); the Stripe webhook's skip rule (every deployment's hostnames); Email
Routing (below). It takes no `--deployment`, reads every deployment's edge secret
(`deploy/env/zone.env.tpl`), and keeps its state in prod's state bucket
(`infra/zone/terraform.tfstate`), reached as prod's deploy user:

```bash
./deploy/run.sh zone plan
./deploy/run.sh zone apply                   # type zone
```

A zone holds one entrypoint ruleset per phase: import an existing late-transform or
custom-rules ruleset before the first apply (`./deploy/run.sh zone import
cloudflare_ruleset.edge_auth zone/<zone id>/<ruleset id>`), and the stack then owns all
its rules.

### Edge authentication

A Cloudflare Worker on anyone's zone can fetch an origin from a real Cloudflare edge with
any `CF-Connecting-IP` it likes. Edge authentication closes that: a Transform Rule on our
zone stamps `X-OpenNova-Edge: <secret>` on every request for a deployment's web
hostnames, with that deployment's own secret (`app-<deployment>/edge_auth_secret`:
staging's hostnames never carry prod's, nor the reverse; the stack refuses two equal
secrets), and nginx forwards `/api/` only for requests carrying it. `secrets init` makes
each deployment's secret, `infra/zone` makes the rules, and web's task definition names
`EDGE_AUTH_SECRET` for the portal container, so a deploy without it in SSM stops before it
registers anything.

**Rotating** (one deployment's), so the rule moves without a window of 403s: nginx accepts
a second value, `EDGE_AUTH_SECRET_PREVIOUS`, during a rotation.

1. In one sitting: in `app-<deployment>`, copy `edge_auth_secret` into a new field
   `edge_auth_secret_previous` and set `edge_auth_secret` to a new value (`openssl rand
   -hex 32`); `--deployment <deployment> secrets sync`; `--deployment <deployment> app
   converge`. nginx accepts both.
2. `./deploy/run.sh zone apply`: the rule stamps the new secret.
3. Delete the `edge_auth_secret_previous` field, `secrets sync --prune`, `app converge`
   (the same `--deployment`).

### Email (support@<domain>)

Mail to `support@<domain>`, to each local part in `email_routes` (default `support`,
`aws-prod`, `aws-staging`: the member accounts' root addresses) reaches one mailbox of
yours through Cloudflare Email Routing (`infra/zone/email.tf`, the zone's):

1. In the vault, item `email`, set `forward_to` to the destination address (`secrets
   init` makes the field as `REPLACE_ME`; `REPLACE_ME` or empty keeps email routing off).
   Not an address at the domain itself, which would loop.
2. `./deploy/run.sh zone apply`: it turns Email Routing on for the zone (Cloudflare adds
   and locks its MX and SPF records; a zone holding MX records of another mail service
   must drop them first), adds the destination address to the zone's account, and makes
   one rule per local part.
3. Cloudflare emails the destination a verification link: click it. Nothing is forwarded
   until then (Email, Email Routing, Destination addresses in the dashboard shows it
   pending). The accounts' root addresses depend on it: a root password reset goes there.

It is receive-only: replies leave from the destination mailbox, under its own address.
The service's own mail leaves through Amazon SES ("Outbound mail (SES)", next). Every
address no rule names is refused (the zone's catch-all stays off). The token's email
permissions are in section 1.

### Outbound mail (SES)

The service mails each new account a welcome and sends the password reset links
(apps/novaworld_common/README.md, "Account email and password resets") through Amazon SES
in its deployment's own account, as `<mail_sender>@<the deployment's site host>`:
`mail_sender = "support"` in `prod.tfvars` sends as `support@<domain>`; staging, when its
var-file sets one, as `<mail_sender>@staging.<domain>`, whose mail and links are
staging's alone. Empty (staging's default) sends nothing; the service then logs each mail
as not sent. `infra apply` (`infra/aws/mail.tf`) makes, in the deployment's account:

- the site host's SES identity with Easy DKIM, its three `_domainkey` CNAMEs in the zone
  (prod's at the apex, staging's under `staging`), and a custom MAIL FROM domain,
  `bounce.<site host>` (its MX to SES's feedback endpoint and its SPF), so the mail passes
  SPF and DKIM aligned with the domain. The apex's MX and SPF stay Email Routing's. The
  records are DNS only; the token's DNS permission covers them;
- an identity for `email/forward_to`, the account's sandbox recipient (below);
- the worker's one mail permission (its task role): `ses:SendEmail` through those
  identities, from the sender alone (web queues the mail as `mail.send` jobs and sends
  none); and `ONNET_MAIL_FROM` and `ONNET_MAIL_REGION` in web's and the worker's
  environment. The links in the mail are `ONNET_SITE_URL`'s, the deployment's own site.

Replies to `support@<domain>` reach the mailbox through Email Routing (above).

**The sandbox.** A new AWS account's SES is in the sandbox: it sends only to verified
addresses and identities, at most 200 a day, so a stranger's welcome mail or reset link is
refused (logged: `MessageRejected`, "Email address is not verified"; the registration
itself works). Each account has its own: once per account, in its console, Amazon SES,
Account dashboard, **Request production access** (mail type Transactional, the site's
URL, and a word on what is sent: a welcome on registration and password resets on
request, to addresses people gave). Until AWS grants it, the `forward_to` identity lets
mail to the maintainer's own address through: AWS mails that address a verification link
after the apply; click it. The console shows each identity's state: the domain's turns
verified once its DKIM records resolve (minutes).

### The first site admin

Register the account on the deployment's site, then add a `bootstrap_admin` field holding
its username to the deployment's app item (`app-<deployment>`), `secrets sync` and `app
converge` (with its `--deployment`). Web promotes the account only while no account is an
admin and logs what it did. Then delete the field, `secrets sync --prune` and `app
converge` (web warns at every boot while the setting is present). Never put the name in a
committed file: a name in a public file is one anybody can register first.

## 5. Deploy

The images publish to GHCR from CI (`.github/workflows/novaworld-images.yml`), under
`image_registry` (`ghcr.io/opennova-net`): one per program, `opennova-novaworld-gate`,
`-lobby`, `-web`, `-fleet`, `-worker` and `-data` (one Dockerfile, a target each); the
portal's nginx, `novaworld-web`; the backup task's, `opennova-novaworld-backup`
(`deploy/backup`); and, from `serve-image.yml` (one Dockerfile, `apps/serve/Dockerfile`,
a target each), `opennova-serve` (the hosted servers) and `opennova-hostd` (hostd, the
machines' agent: the server's runtime with hostd, the docker CLI, the AWS CLI and
nftables). Make each package public once: the tasks and the machines of both accounts
pull anonymously (the task execution role holds no registry credentials, and hostd's
task has no execution role at all).

```bash
./deploy/run.sh --deployment staging app deploy --tag sha-abc1234 --serve-tag sha-def5678
./deploy/run.sh --deployment prod app deploy --promote staging      # type prod
```

`app deploy`:

1. resolves each image to its digest now: `--tag` sets the six programs', the portal's
   and the backup task's (all built from one commit), `--image NAME=TAG` one of them
   (`gate`, `lobby`, `web`, `fleet`, `worker`, `data`, `portal`, `backup`, `serve`, `hostd`;
   `NAME=@sha256:<digest>` pins a digest as it is), `--serve-tag` and `--hostd-tag` the
   machines'. An image given no tag keeps the current release's digest (while it is of
   that image's own package); the first release takes `latest` for any tag not given. `--promote staging` (prod alone) takes, instead,
   exactly the images staging's current release runs, by digest, read in staging's
   account with staging's own credentials (`app images` prints them; `--images-from
   FILE|-` takes the same JSON, the deploy workflow's way), and records which release it
   promoted;
2. registers each program's task definition revision: Terraform's task definition (the
   template, `infra.env` names it) with the release's images, the serve image and the
   hostd image in the environment of the programs that read the fleet's policy (web, the
   fleet, the worker; `--no-hosting` turns their provider off; the worker's `docker`
   driver's machines run the hostd image, and the fleet's health holds each machine's
   agent against it), every secret the template names (SSM must hold each) and each
   optional one its label names that SSM holds; hostd's revision (its own template, the fleet cluster's daemon)
   takes the hostd image as its container's. A program whose
   revision would run exactly what it runs now keeps it, so a release that changes only
   web's image restarts web alone and never a lobby session;
3. on prod, prints the images and asks (type `prod`, or `--yes`);
4. rolls the services in turn: worker, data, hostd, fleet, web, lobby, then gate, the
   players' way in, last. It follows each service's new deployment until ECS calls it
   COMPLETED (its new tasks healthy, the old stopped) or FAILED: ECS's deployment circuit
   breaker rolls a deployment whose new tasks never turn healthy back to the old
   revision, after 10 failed launches, so the wait is bounded at 40 minutes
   (`OPENNOVA_ROLL_TIMEOUT`, seconds). A failed or unfinished one stops the deploy,
   leaving the rest on what they ran. A service at no task before a release names its
   program (and one `app down` recorded no count for: a program added since) starts at
   its `desired` count, and its floor is raised to its `min` (the fleet, up before any
   lobby, starts no hosted server until a lobby runs:
   `apps/novaworld_common/hosted/README.md` "No lobby live"). hostd rolls before the
   fleet program it reports to: its deployment replaces at most half the machines' agents
   at once (no game server restarts: the containers are docker's, not the agent's), and
   once it completes the deploy waits up to 3 minutes for every ready machine's agent to
   report the release's hostd image (the admin fleet view's `agent_image`). If one does
   not, or hostd's deployment fails, the deploy points hostd back at the revision it ran
   and stops, the fleet program still on its own, so agents and fleet never run two
   releases' protocols apart for long
   (an agent of another protocol is held, heard and acted on for nothing, 10 minutes
   before its machine drains). With no machine running hostd there is no machine to
   confirm; when the service cannot be asked (a first release, the cutover: no web or no
   fleet program yet; or ECS Exec into web refused) nothing is confirmed and the log says
   which failed (a refused call by name, the admin API's status). Either way the deploy
   goes on;
5. once every program runs it, registers the backup task's revision (the nightly run
   takes the newest) and records the release: its manifest (the images by digest, each
   revision, the previous release, the release it was promoted from) goes to
   `s3://<ops bucket>/releases/<id>.json` and to `/opennova/<deployment>/release`. A
   deploy that stops records nothing: the release it was on stays the deployment's, and
   `app rollback --to <that id>` puts the programs it already moved back on it.

How each program's tasks are replaced: every program starts the new task first and stops
the old one once it is healthy, so a bad image stops nothing that runs (the fleet's
new task waits as the standby, then leads once the old one lets the lock go; the
balancer sends new lobby flows to the new lobby task once it passes, then drains the old
one for 10 s before ECS stops it). A lobby's restart is the one that shows: its players
sign in again through the gate, and the servers listed on it, hosted or retail players'
own, leave the browser for up to about 4.5 min (a host's session drops the silent lobby
after 240 s and reconnects to the one lobby address), then list again with no restart and
their matches going on. The jobs (the worker's, the data program's) and the fleet's
rows wait out a restart in the database. hostd is the exception: one task per machine
(a DAEMON service, never two agents on one machine), so ECS stops at most half the
machines' agents, then starts their new ones (`minimumHealthyPercent` 50); a machine's
servers run on meanwhile, and its agent's sync gap stays well under the 60 s after which
the fleet program takes a silent machine for lost. hostd has no count: `app down` and
`app up` leave it running, and its machines with it.

```bash
D='--deployment staging'                              # or prod
./deploy/run.sh $D app releases                       # the newest 20, the current one starred
./deploy/run.sh $D app images                         # the current release's images, as JSON
./deploy/run.sh $D app rollback                       # the previous release's revisions
./deploy/run.sh $D app rollback --to <id>             # or a named one's
./deploy/run.sh $D app converge                       # the current release again (new secrets, an infra apply)
./deploy/run.sh $D app status                         # every program's counts, rollout and revision
./deploy/run.sh $D app restart web                    # new tasks for a program (every program without one)
./deploy/run.sh $D app logs lobby --follow            # CloudWatch, a stream per container and task
./deploy/run.sh $D app logs hostd                     # every machine's agent (the machines' log group)
./deploy/run.sh $D app shell fleet                    # a root shell in a running task (ECS Exec)
./deploy/run.sh $D app shell web portal               # web's second container, the portal's nginx
./deploy/run.sh $D app down                           # every program to 0 tasks (the counts recorded; hostd runs on)
./deploy/run.sh $D app up                             # the recorded counts back (a program added since: its desired)
```

`app converge` is a release of the same images: Terraform's task definitions as they are
now (after an `infra apply` that changed one: Terraform makes a new template, and the
services keep their revision until a deploy) and the secrets SSM holds now (after a
`secrets sync` that added or removed one; a changed value alone needs no more than `app
restart`, since a task reads its secrets as it starts). On prod, `app deploy`, `app
rollback`, `app converge`, `app restart` and `app down` ask first: they go ahead once you
type `prod`, or with `--yes`, and a run with no terminal refuses without it (as `infra
apply`, `secrets sync`, `db bootstrap`, `db reset-db`, `backup restore`, `backup import
-`, `stripe setup`, `fleet end` and `fleet roll` do). `app down` also holds Application
Auto Scaling off until `app up`; an `infra apply` meanwhile puts no service back
(Terraform leaves the counts to the scaler). `--no-hosting` deploys with hosted game
servers off (the kill switch, section 7).

**Rolling back.** `app rollback` points every service back at the previous release's
revisions (or `--to` a named one's), in the same order, and once they all run them makes
it the current release; the revisions stay registered, so any release made since the
deployment's account was built can be rolled back to, with two limits: a rollback across a
migration is the database's business (section 6), and a release from before the program
split (its programs name no fleet and no data program: the controller's) is refused, since
no service runs its controller any more; past the split, fix forward. The nightly backup runs the current
release's backup revision (its image pinned by digest): `app deploy` and `app rollback`
point its schedule at it as they record the release, and an `infra apply` reads the same
from the release (never the family's newest revision, which an apply's template,
`latest`, may be). Before the first release the schedule is off.

### The deploy workflow

`.github/workflows/deploy.yml` runs the same `app deploy` from GitHub Actions with no
vault and no stored key: each run assumes its deployment's GitHub deploy role (the org
stack makes one per account, trusting only this repository's environment of the same
name) through GitHub's OIDC. `infra/github` makes the two repository environments,
`staging` (no reviewers) and `prod` (its required reviewers approve each run; protected
branches alone), each with the variables `AWS_DEPLOY_ROLE_ARN`, `AWS_REGION` and
`AWS_ACCOUNT_ID` from the vault's `aws-<deployment>` items. Run it from the Actions tab:

- `staging` with the tags (the toolbox's `--tag`, `--image`, `--serve-tag`,
  `--hostd-tag`);
- `prod` with no tag: a first job, in the `staging` environment, reads staging's current
  release (`app images`), and the second, in `prod` once approved, deploys exactly those
  images (`--images-from -`). Prod never runs images staging has not.

A deployment's first release runs from the toolbox, never the workflow: it raises each
program's task-count floor (Application Auto Scaling), which the role may not. The role
reads the down marker `app down` leaves, so a run while the programs are down starts
none of them.

A role may read its deployment's settings and the names of its secrets (never a value),
write its release manifests and point its release, register task definition revisions
(handing them the stack's task roles alone), point its cluster's services, its fleet
cluster's hostd and its backup schedule at them, open ECS Exec into its web container
(the admin API on the task's loopback: after hostd's roll, `app deploy` asks which hostd
image each machine's agent reports, as an operator's run does), and nothing else. A run
whose exec is refused (the org stack not yet applied) logs the refused call, confirms
nothing, and goes on.

## 6. Backups

Two independent layers:

- **RDS point-in-time recovery**, 14 days, kept even past a deletion of the instance
  (and a final snapshot at one). `./deploy/run.sh --deployment <deployment> db snapshot`
  takes a manual snapshot now (before risky work); RDS restores a point in time or a snapshot into a new
  instance (the RDS console, or `aws rds restore-db-instance-to-point-in-time`), which you
  then adopt by pointing the stack at it.
- **pg_dump**, the backup task (`deploy/backup`, `infra/aws/backup.tf`): a custom-format
  dump to S3 nightly at 03:00 UTC, kept 90 days, independent of the database instance.
  EventBridge Scheduler runs it as a Fargate task with the programs' `DATABASE_URL` and
  `DATABASE_PASSWORD`; the dumps go under `novaworld/` in the deployment's own
  operations bucket, which its backup task alone writes.

```bash
./deploy/run.sh --deployment staging backup now     # a dump now (a one-off task)
./deploy/run.sh --deployment staging backup list    # the dumps
./deploy/run.sh --deployment staging backup restore novaworld/novaworld-<ts>.dump --yes-overwrite-database
```

`backup restore` stops every program (`app down`), and in a one-off task dumps the
database as it stands (a `novaworld-pre-restore-<ts>` copy) and restores the dump in one
transaction (`pg_restore --clean --if-exists --single-transaction`), then starts every
program again (`app up`), whose boot applies any migration the dump predates (a failed
restore starts them again too). A program older than the dump refuses an edited or
unknown migration rather than run on it. The task reads its own deployment's dumps
alone; another deployment's dump comes in by `backup import` first, and only one way: prod's
into staging (`--from-deployment prod`), never staging's into prod. On prod the restore
asks for `prod` besides `--yes-overwrite-database`.

**The restore drill**, once a quarter: the newest prod dump into staging, sign in on the
staging site and check an account and a CD key:

```bash
./deploy/run.sh --deployment prod backup list
./deploy/run.sh --deployment staging backup import --from-deployment prod novaworld/novaworld-<ts>.dump
./deploy/run.sh --deployment staging backup restore novaworld/novaworld-import-<time>.dump --yes-overwrite-database
```

The import reads prod's dump with prod's credentials and writes staging's copy with
staging's, the dump passing through the toolbox: no principal of one account reads the
other's bucket.

### Resetting the service database

Once billing is live the database is durable: migrations are append-only, and `db
reset-db` refuses while any subscription that grants hosting (active, trialing, past_due)
is recorded:

```bash
./deploy/run.sh --deployment staging db reset-db --yes-drop-database
```

It dumps the database first (`--no-backup` skips it), stops every program, drops the
database and makes it again in a one-off task (the same `CREATE DATABASE` as `db
bootstrap`), and starts every program again, the first to boot applying every migration.
`--orphan-subscriptions` resets anyway: those customers keep paying with no account
linked here, so cancel them in Stripe first. Everything goes with the reset: accounts,
sessions, host keys, CD keys, DFX2 activations, billing links; make the first site admin
again (section 4). On prod it asks for `prod` besides `--yes-drop-database`.

## 7. Hosted game servers

An account with a hosting entitlement (a monthly support tier, or an admin's comp grant
of servers and own machines, the admin user page) starts servers from the site's My servers page, picking a data set
(JO, DFX2 or OpenNova: a product's game files, which admins upload on the site's Game
data page, below) and, for JO, its Escalation expansion. The server is always the public
`opennova-serve` image, started with `/game <title>` (and `/exp <code>`) over the data
set's files, which its machine fetches from the store.

The service places each server on a machine it launches itself (its EC2 machine driver,
in the worker's jobs, with the worker's task role), in either public subnet:

- **shared machines**, `t3a.medium` with unlimited CPU credits, holding up to 6 servers
  (`shared_slots`; each slot's 512 MiB memory limit, so 3 of its 4 GiB) of any accounts, packed best-fit; an idle one ends after 30 minutes
  (`fleet_idle_minutes`);
- **own machines**, `c7a.medium`, exactly one server: the own-machine add-on.

Slot k of a machine is game UDP `32768+k` (to players) and admin TCP `32800+k` (to the
fleet program alone); hostd, the agent on each machine, runs one container per slot and
reports to the fleet program's internal listener (`fleet.<deployment>.opennova.internal:8070`,
its Cloud Map name, admitted from the machines alone). Each machine boots Amazon's
ECS-optimized Amazon Linux 2023 image, pinned by name (`fleet_ami`), and joins the
deployment's fleet cluster (`opennova-<deployment>-fleet`) as a container instance;
hostd is that cluster's DAEMON service, one task per machine, which `app deploy` rolls
(section 5). A machine holds no volume of game data: its hostd fetches the trees its
servers pin from the store (below), once each, onto its root volume
(`machine_root_volume_gb`, 40 GiB), and keeps each while a server needs it. Machines are
terminated, never stopped. No machine has an SSH key or database credentials;
break-glass is ECS Exec into its hostd (`fleet shell`). At most 2 shared and 2 own machines run at
once (`fleet_max_shared`, `fleet_max_own`); `ONNET_HOSTED_CAP`, what that fleet holds at
its most (`fleet_max_shared` x `shared_slots` + `fleet_max_own`: 14 at the defaults, 7 on
staging), caps the servers. The same sizes are what the site sells: a checkout past
`fleet_max_shared` x `shared_slots` shared places, `fleet_max_own` own machines or
`fleet_max_addresses` static addresses (0 by default: the static-address add-on off;
"Turning it on", step 9) is refused as sold out. `infra plan` and `infra apply` warn when a
plan lowers one of the four: an account already sold more keeps it, and the admin's
attention list shows the shortfall.

### The game data

Admins create and upload data sets on the admin page **Game data** (`/admin/data-sets`;
ADR 0064). A data set is one product: of kind base (JO, DFX2, OpenNova: its title and the
lobby it lists in) or expansion (Escalation, code `jox01`, under JO), with the login and
command file retail `UPDATE.EXE` asks for when OpenNova's update service serves it. Its
data is a **tree**, one immutable version of its files: **Upload data** takes an install's folder
from a folder picker, keeps only the files `opennova-serve` reads, sends them through the
site in 32 MiB parts (each a request to web, which puts it in the bucket; the bucket needs
no CORS rule), and **Finalize** has the data program check the names and sizes and list the
missions, which are the data set's map rotation (there is no configured one):

| File | Kind | Why |
|---|---|---|
| `resource.pff`, `localres.pff`, `language.pff` | base (required) | the retail boot table it mounts |
| `version.txt`, `score.ini`, `_NSTMOUT.TXT`, `gt.ssc` | base (when present) | the version joiners compare; the scoring table; the session timeout override; the game types |
| `<code>.pff`, `<code>L.pff`, `version.txt` | expansion (required) | the expansion's archives and the checksum joiners compare |
| `gt.ssc` | expansion (when present) | its game types |

A data set's **current tree** is what new servers start on; a running server is pinned to
the trees it started on until its owner presses **Update** on its page (the page says an
update waits), or an admin's **Update servers** does it for every server of the data set,
and a crash restart or a fleet roll keeps the pins. A **release** (a 1SFS package built
with `opennova-update-tool build`, uploaded on the same page) validated and published is
applied to the data set's baseline tree, which makes its next current tree, and the update
service serves the package to retail `UPDATE.EXE` ("The update service", below).

The store is the deployment's operations bucket under `data/` (one bucket per deployment,
so the prefix carries no deployment): `trees/<tree id>/<file>` with its `manifest.json`
(each file's name and size, the version line), `updates/<sha256>.sfs` (validated packages)
and `updates/upload-<release id>.sfs` (before validation). `ONNET_OBJECT_STORE_URL`
(`s3://opennova-<deployment>-ops-<account id>/data`) and `ONNET_OBJECT_STORE_REGION` name
it to web, the fleet, the worker and the data program. The machines' hostd fetches a tree
with `aws s3 sync` under its task role, which reads `data/trees/` and nothing else of
the bucket, and checks the names and sizes against the manifest; web's and the data
program's task roles read, write and delete under `data/` alone. The bucket aborts an upload abandoned for 7
days, and keeps a deleted object's old version 7 days (a tree's) or 30 (a package's).
Nothing of the game data
goes in git, an image or the vault, and nothing of it crosses between the accounts: each
deployment's admins upload it on its own site, into its own bucket.

### Turning it on

1. **The boundaries.** The task roles' store access needs `s3:DeleteObject` and
   `s3:ListBucketMultipartUploads` in `infra/aws/iam/opennova-role-boundary.json`, and the
   machines' two roles (the instance role, hostd's task role) their own boundary,
   `opennova-machine-boundary.json` (the ECS agent's calls on the fleet cluster: its
   registration, its polls, its state changes; the machines' and servers' logs; the
   trees' reads), which the org stack makes in both accounts, with the deploy user's leave
   to write a role under it and the deploy workflow's to roll hostd, before the apply below
   (`./deploy/run.sh org plan`, then `org apply`; `infra/aws/iam/README.md`, "The
   boundaries"); applied after, every store call of web and the data program is refused
   until it is, and the apply cannot make the machines' roles, so no machine joins the
   fleet cluster.

2. **`hosted_enabled = true`** in the deployment's var-file (`infra/aws/<deployment>.tfvars`;
   prod's and staging's say so). Off, fleet.tf creates nothing and the programs run with
   hosting off.

3. **Check the vCPU quota.** The machines alone use the account's On-Demand Standard
   instances quota (L-1216C47A; the programs run on Fargate, whose quota is its own):
   each `t3a.medium` 2, each `c7a.medium` 1. The defaults need 6 (2 x 2 + 2 x 1), in
   each deployment's account. Request 32 in the Service Quotas console to leave room for
   a roll:

   ```bash
   ./deploy/run.sh --deployment staging aws service-quotas get-service-quota --service-code ec2 --quota-code L-1216C47A
   ```

4. **Plan, then apply.** The plan adds the fleet cluster (`aws_ecs_cluster.fleet`), the
   hostd daemon (`aws_ecs_service.hostd`, its task definition and task role, which reads
   the trees), the two launch templates (`aws_launch_template.fleet["shared"]`, `["own"]`,
   from the AMI `fleet_ami` names), the machines' security group and its rules, the
   machine role (its ECS agent's calls and the machines' logs) and the worker's fleet
   policy, web's and the data program's store policies and the game data's lifecycle
   rules; it changes web's, the fleet's, the worker's and the data program's task
   definitions (the fleet's policy and the store's URL and region; the worker's alone the
   machine driver's own settings; `app converge` takes them). The update service's
   listeners and record (below) are there whether or not hosting is on. hostd runs
   nowhere until a machine registers, and only the worker launches machines.

5. **Deploy**, so the programs read them, pin the serve image and roll hostd onto the
   fleet cluster: `./deploy/run.sh --deployment staging app deploy --serve-tag sha-<serve>
   --hostd-tag sha-<serve>` (on prod, the next `--promote staging`). Web, the fleet and
   the worker log `[boot] hosted servers: ec2, serve image ...`; the worker also its
   driver's `[boot] hosted servers: ec2 in <region>, subnets ...` and `... the machines
   join ECS cluster opennova-<deployment>-fleet` (a worker whose driver settings are
   incomplete does not boot: `app logs worker` names what is missing).

6. **Upload the data.** On Game data, make the data sets (JO in `jop_2_consumer`,
   Escalation `jox01` under it, DFX2, OpenNova); **Upload data** on each from its install's
   folder, then **Finalize**. A data set is offered to owners once its current tree is
   ready; until then a server of it fails with "its data set has no data" (not counted as
   a crash). Each deployment's admins upload into that deployment alone: staging's data
   never reaches prod's bucket, nor the other way.

7. **The machines' AMI.** `fleet_ami` in the var-file names the exact image (Amazon's
   ECS-optimized AL2023, x86_64, its full name), so a new one shows in the plan and in
   git. `fleet status` says when Amazon has published a newer one; bump it in
   `staging.tfvars` first (`infra apply`, then `fleet roll`, smoke test F), then in
   `prod.tfvars` ("A machine image", below).

8. **Subscribe, or grant.** An account hosts once it subscribes to a support tier, or an
   admin gives it a comp grant (servers, and own machines, on the admin user page). Run smoke tests F
   to J ("Smoke tests", at the end).

9. **Static addresses** (optional): the add-on that gives an own machine a static IPv4
   address and a name, `<name>.play.<domain>` (staging's `<name>.play.staging.<domain>`).
   Off while `fleet_max_addresses` is 0, the default. In this order:
   - **The Elastic IP quota, first.** Each address is an Elastic IP of the deployment's
     account, under the quota L-0263D0A3 ("EC2-VPC Elastic IPs", per Region): 5 by default,
     and the load balancer holds 2 of them in each account. Request 10 in each account in
     the Service Quotas console, and wait until it is granted, before raising
     `fleet_max_addresses` (an allocation past the quota fails, and the address shows
     failed):

     ```bash
     ./deploy/run.sh --deployment staging aws service-quotas get-service-quota --service-code ec2 --quota-code L-0263D0A3
     ```

   - **The role boundary** (step 1) holds the worker's address and Route 53 rights: `org
     apply` with this checkout's `opennova-role-boundary.json` before the apply below.
   - **The price.** `stripe setup` makes the add-on's price and writes
     `price_static_address` ("Monthly support tiers (Stripe)"), then `secrets sync`; with
     `fleet_max_addresses` above 0 and no `STRIPE_PRICE_STATIC_ADDRESS`, billing stays off.
   - **`fleet_max_addresses`** in the deployment's var-file (the addresses it sells, at
     most 64), then `infra apply`: the plan makes the play zone (`aws_route53_zone.play`,
     a Route 53 public zone of the deployment's own) and the four Cloudflare NS records
     that delegate it to Route 53 (`cloudflare_record.play_ns`), both `prevent_destroy`,
     and names the zone to web, the fleet and the worker (`ONNET_HOSTED_ADDRESS_ZONE`) and
     its id to the worker alone (`ONNET_HOSTED_ADDRESS_ZONE_ID`; `app converge` takes them).

   The zone costs $0.50 a month. An address costs $3.65 a month while it is idle, and
   nothing more while its server runs (it takes the place of the machine's own public
   IPv4).

### The update service

Web serves retail `UPDATE.EXE` (ADR 0061) on the updater's four TCP ports (20044, 20556,
21068 and 21580: `update_ports`, hardcoded in the updater) behind the load balancer, from
everyone (`allowed_cidrs`): a listener per port, all four forwarding to one target group on
web's server (ECS admits five target groups per service, and web holds three), so web
serves every port and a task sees the updater on the first. Where the updater goes is the
data set's: with `retail_updater` on, its SERVER.LST names `ftp.novaworld.com`, which the
Switcher's managed block maps to the gate's address, the balancer's zone-a address, so a
switched client updates from here; with it off, `update.<domain>` (`update.<deployment>.<domain>`
off prod), an unproxied CNAME to the balancer the deployment's apply makes with the zone
(web's `ONNET_UPDATE_HOST`; without a zone it is empty and the listener stays off). The
packages are the published releases of the Game data page; web fetches one from the store
the first time the updater asks for it, into its task's storage.

### Fleet commands

```bash
D='--deployment staging'                        # or prod
./deploy/run.sh $D fleet status                # EC2's view, the fleet cluster's (ECS), the AMI, then the service's:
                                               #   what needs you, one line per machine
./deploy/run.sh $D fleet logs server <id>      # a server's newest start's output (CloudWatch; below)
./deploy/run.sh $D fleet logs machine <id>     # a machine's hostd
./deploy/run.sh $D fleet console <machine>     # a machine's serial console (EC2), while EC2 lists it
./deploy/run.sh $D fleet shell <machine>       # a root shell in the machine's hostd (ECS Exec)
./deploy/run.sh $D fleet drain <machine>       # no new server there; its servers move
./deploy/run.sh $D fleet end <machine>         # end it (its servers move first)
./deploy/run.sh $D fleet roll --dry-run        # which machines a roll replaces, and why
./deploy/run.sh $D fleet roll                  # replace them: their servers move to new machines
```

`fleet status`, `fleet logs`, `fleet drain`, `fleet end` and `fleet roll` call the
service's admin API inside a web task over ECS Exec, from the task's loopback, with the
admin token the web container holds. `fleet status` prints what needs you (the site's
Attention list: the fleet's findings, danger first, and the firing alarms) and one line per
machine (its state and slots, its address, its last report, its CPU, memory and disk, its
hostd, and whether it is older than the release or held). `fleet roll` is for a new AMI, launch template or user data alone: the
service's plan replaces each machine taking servers whose image is older than the pinned
AMI and keeps the rest (`--dry-run` prints it and stops), and the replaced machines'
servers move without counting restarts; a new hostd reaches every machine with `app
deploy`, no roll. On prod `fleet end` and `fleet roll` ask first (type `prod`, or
`--yes`). A deployment's worker launches and ends its own account's machines alone
(the fleet program decides, the worker's `machine.*` jobs call EC2, each launch refused
past `fleet_max_shared` or `fleet_max_own`): the other account's are out of its reach.
Never drain a container instance of the fleet cluster in ECS (the console's or `aws ecs
update-container-instances-state`): that stops its hostd, and the service takes the
machine for lost; `fleet drain` and `fleet end` are the ways a machine leaves.

### The kill switch

The admin fleet view's **stop all** stops every live server and the service ends the
machines. To keep hosting off across deploys, deploy with `app deploy --no-hosting`. If
the service is down, `fleet status` lists the machines and `./deploy/run.sh --deployment
<deployment> aws ec2 terminate-instances --instance-ids <machine> ...` ends them; the service ends any machine
it finds without a server at its next pass anyway. To turn hosting off for good: stop all,
deploy with `--no-hosting`, `hosted_enabled = false` in the var-file, `infra apply` (the
game data stays in the bucket).

### Operating notes

- **A machine image.** The launch templates launch from the AMI the var-file's
  `fleet_ami` names (Amazon's ECS-optimized Amazon Linux 2023, x86_64, by its exact name,
  owner `amazon`), and the worker may launch that image alone. It carries docker and the
  ECS agent; a machine's user data writes its token, its id and the ECS agent's settings
  (its cluster, its `machine-id` and `kind` attributes) and nothing else, and the ECS
  agent starts the machine's hostd task once it registers. `fleet status` names
  Amazon's newest image beside the pinned one. To move to it: the new name in
  `staging.tfvars`, `infra apply` (the launch templates' new version; new machines boot
  from it), `fleet roll --dry-run`, `fleet roll` (the machines on the older image are
  replaced, their servers moving without a restart counted), smoke test F; then the same
  in `prod.tfvars`. Going back is the old name and the same steps. The ECS agent never
  cleans up images it did not pull (`ECS_ENABLE_UNTRACKED_IMAGE_CLEANUP` stays off, so the
  servers' serve images are hostd's to prune), and runs no privileged container.
- **New data, or a new data set.** On Game data: a new data set is a row, new data a
  tree uploaded on it (or a release published), and new servers start on it at once; a
  running server moves to it when its owner presses Update (or the admin's Update servers
  does), which restarts it on the new tree. Nothing to apply, no roll: a fleet roll keeps
  every server's pins, so a rolled server comes back on the data it ran on. Staging
  first, then prod's own upload.
- **A stuck upload.** The page holds an upload's parts until Finalize; a page closed
  mid-way leaves the tree `uploading` with its parts in the bucket. Delete the tree on the
  page (its uploads are aborted), or leave it: the bucket aborts them after 7 days. A tree
  `failed` says why in its row; delete it and upload again.
- **Pruning.** The data program's `tree.prune` (hourly, and after each finalize, validate and
  apply) removes a tree that is neither current nor the baseline and that no live server
  pins, an hour after it stopped being any of those; a running server's data is never
  deleted under it. A deleted release's package goes with the release.
- **A machine's logs.** Every server's start writes CloudWatch
  `/opennova/<deployment>/servers` (14 days), a stream per start, `<server id>/g<generation>`
  (`opennova-serve`'s standard output and error, its status page off: why a server exited;
  the server's start rows name each stream), and every machine's agent writes
  `/opennova/<deployment>/machines` (30 days), one stream per hostd task,
  `hostd/hostd/<task id>` (ECS's awslogs driver: the agent's lines, its starts and
  failures, docker's refusals; the admin fleet view's "Machine N" names the machine). The
  deploy user reads them, with no shell on the machine, by the streams the service's rows
  name (the site's server and machine pages read the same):

  ```bash
  ./deploy/run.sh $D fleet logs server 34                    # server 34's newest start: the last hour
  ./deploy/run.sh $D fleet logs server 34 --start 2 --since 1d   # its start 2 (the Logs tab names them), further back
  ./deploy/run.sh $D fleet logs machine 12 --follow          # machine 12's hostd, live
  ./deploy/run.sh $D app logs hostd --since 1d               # every machine's agent, an ended machine's among them
  ```

  The streams outlive the server and the machine. A server the service restarts after a
  crash starts again as a new start, a stream of its own. While the service does not
  answer, `fleet logs server` reads the streams by their names (start N's, or every
  start's, each line naming its stream) and `fleet logs machine` the machine's running
  hostd task. A machine whose hostd never started (its user data failed, or its ECS agent
  never registered: `fleet status` shows no container instance for it) writes nothing
  there: its serial console says why (`fleet console <machine>`, by the service's id or
  EC2's) while EC2 still lists it.
- **A refused AWS call.** The service logs each failure (`app logs worker`); for an
  `UnauthorizedOperation`, `./deploy/run.sh --deployment <deployment> aws sts decode-authorization-message
  --encoded-message <message>` names the condition that refused it.

## Monthly support tiers (Stripe)

Billing runs on Stripe Checkout and the Customer Portal: the server makes the sessions
and redirects to Stripe, so no card data and no publishable key touch our pages. Test
mode serves staging (`stripe-test`), live mode prod (`stripe-live`).

**What a mode holds** once `stripe setup` (below) ran in it: the products and their
monthly USD prices, found by lookup key (`opennova_tier1_monthly` $5,
`opennova_tier2_monthly` $10, `opennova_tier3_monthly` $15: 1, 2 and 3 hosted servers;
`opennova_own_machine_monthly` $45, the add-on that puts one of the account's servers on
a machine of its own, about what a `c7a.medium` costs; `opennova_static_address_monthly`
$5, the add-on that gives one own machine a static IPv4 address and a name under the
deployment's play zone, which covers the address's $3.65 and Stripe's fee on the charge),
their ids in the item's `price_tier1`..`price_tier3`, `price_own_machine` and
`price_static_address` fields (`secrets sync` syncs billing's parameters only while every
one is filled: when a new one appears, `stripe setup` fills it); the Customer Portal
configuration tagged `opennova=support_tiers`; and the webhook endpoint (live
`https://<domain>/api/billing/webhook`, test `https://staging.<domain>/api/billing/webhook`)
on the API version the server pins (`2025-03-31.basil`) with the 11 events it handles
(`checkout.session.completed`, `.expired`; `customer.subscription.created`, `.updated`,
`.deleted`, `.paused`, `.resumed`; `invoice.paid`, `.payment_failed`,
`.payment_action_required`; `customer.deleted`), its signing secret in the item's
`webhook_secret`.

**The toolbox.** The one field you fill is the item's `secret_key` (Developers, API
keys); the rest is the toolbox's:

```bash
./deploy/run.sh --deployment staging stripe check   # read alone: the item and its objects
./deploy/run.sh --deployment staging stripe setup   # make what is missing (idempotent)
./deploy/run.sh --deployment prod stripe check
```

The mode is the deployment's: prod live (`stripe-live`), staging test (`stripe-test`),
each on its own site; no command crosses them. On prod `stripe setup` asks first. `stripe setup` makes what the mode lacks and leaves
the rest: a product and a monthly price per lookup key (`--tier1`, `--tier2`, `--tier3`,
`--own-machine`, `--static-address` take other amounts in US dollars: an amount that
differs from the current price makes a new price, which takes the lookup key and goes
first in the field, the old one kept after it, since the first is sold and the rest still
grant); the price ids into the item; the portal configuration (customers switch among the
three tier products, an upgrade prorating at once and a downgrade waiting for the
period's end, quantity changes off; cancel any subscription at the period's end, a tier,
one own machine or one static address, since each add-on is a subscription of its own
(quantity 1) bought through Checkout, and a static address's Cancel on the site opens the
portal at that subscription's cancellation; and update the card, the email and the
address and see invoices; the privacy and terms links are `https://<domain>/privacy` and
`/terms`, the return link `https://<web host>/me/support`), brought to those settings
when it exists; and the
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
   privacy, refunds, `web/src/pages/legal/`, drafts until you write them). Statement
   descriptor: OpenNova's is `OPENNOVA`.
2. **Public details** (Settings, Public details), in each mode: the business name, the
   support email `support@<domain>` (section 4, "Email"), the website
   `https://<domain>`, the terms of service URL `https://<domain>/terms` and the
   privacy policy URL `https://<domain>/privacy`. Checkout asks every buyer to accept
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

Then `./deploy/run.sh --deployment <deployment> secrets check` (the item reads `OK`),
`secrets sync`, `app converge`: the server logs billing on; with an incomplete set it logs "billing off".
**Check delivery**: the endpoint's "Send test webhook" must answer 2xx. A challenge page
means Cloudflare stopped it: the webhook's custom rule skips the WAF's rules
(`infra/zone/edge.tf`), but Bot Fight Mode cannot be skipped by a rule, so keep it off
(section 1). On a plan without some skippable phase or product the apply names it: drop
it from `infra/zone`'s `cloudflare_webhook_skip_phases` / `_products`.

## GitHub

The GitHub organization `opennova-net` is managed by `infra/github`, all of it: the
organization's settings, members and Actions permissions, every repository (settings,
topics, default branch, vulnerability alerts), `opennova`'s `master` protection and its
Actions secrets. A change made in GitHub's settings is drift: the next `github plan` shows
it, and `github apply` puts it back. Make the change in `infra/github` instead. The
repositories are never destroyed by Terraform (`prevent_destroy`; one dropped from the map
on purpose is archived, not deleted).

CI's build and test legs run on RunsOn's runners in the ci account (section "CI
runners"), every other job on GitHub's standard hosted runners (`windows-latest`,
`ubuntu-latest`), free for a public repository; the stack makes no runners.

The stack is the organization's, not a deployment's: the toolbox runs it without
`--deployment`, and its state lives in prod's state bucket (`infra/github/terraform.tfstate`,
reached as prod's deploy user). It also makes the repository's two deploy environments,
`staging` and `prod` (prod's with the organization's admins as required reviewers and
protected branches alone), and their variables (`AWS_DEPLOY_ROLE_ARN`, `AWS_REGION`,
`AWS_ACCOUNT_ID`) from the vault's `aws-<deployment>` items (section 5, "The deploy
workflow"). What stays out of this public repository comes from the vault:

- **`github-terraform`**, fields:
  - `token`: a fine-grained personal access token, resource owner **opennova-net**, with
    organization permissions **Administration** and **Members** (each Read and write)
    and, on **All repositories**, **Administration**, **Environments** (the deploy
    environments' variables) and **Secrets** (Read and write).
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
- **`stripe-test`**, field `restricted_key` (`secrets init` adds it as `REPLACE_ME`): the
  value of the `STRIPE_TEST_SECRET_KEY` secret, which test-linux hands the billing
  end-to-end test (`billing_stripe_e2e`) as `ONNET_STRIPE_TEST_SECRET_KEY`. The test makes
  a customer, Checkout sessions and a subscription in Stripe's test mode and deletes the
  customer at the end. Fill it with a restricted key of the test mode with the server
  key's permissions (Developers, API keys, Create restricted key: **Customers**,
  **Checkout Sessions**, **Subscriptions** and **Customer portal** Write, **Prices** and
  **Products** Read; Stripe's 403 names one that is missing); then `github apply`. Never
  the item's `secret_key` (staging's): the local stack takes the same restricted key
  (`deploy/env/billing.dev.env.tpl`). A live key fails the test. While the field holds
  `REPLACE_ME`, or for a fork's pull request (which gets no secret), the test skips.

An org admin makes the tokens, or approves them if the organization reviews fine-grained
tokens. Then:

```bash
./deploy/run.sh github plan
./deploy/run.sh github apply            # shows the plan, then asks for github
```

`infra/github/imports.tf` adopted what existed before the stack. A new repository, member
or secret is a change to the stack: add it, plan, apply.

## CI runners

`ci.yml`'s build and test legs (test-core, test-retail, test-linux, both gdext builds, the
Godot package) run on [RunsOn](https://runs-on.com)'s runners in the Organization's third
account, `opennova-ci`, which holds them alone (`infra/ci`). Each job gets an EC2 instance
of its own with 16 vCPUs, spot unless spot is short (then on demand), launched when GitHub
queues the job and ended when it finishes: nothing runs between jobs. The Godot tests (one
test at a time, so more cores buy nothing) and every other job stay on GitHub's standard
runners, free for a public repository, and so does every job of a pull request from a fork.

What it costs: the instances by the minute (us-east-1 spot, about $0.014 a minute for a
16-vCPU Windows instance with its license and $0.005 for Linux, against $0.082 and $0.042
for GitHub's 16-core larger runners), about $15 a month for RunsOn's control plane (a small
Fargate task, an API Gateway, Lambdas, queues, tables and secrets), the artifacts' upload
to GitHub (about 0.3 GB a run, at $0.09 a GB), the legs' caches in RunsOn's S3 bucket
(Magic Cache: about 2.2 GB of compiler caches a `master` push, kept 10 days, about $10 a
month at twenty pushes a day and no transfer charge), and RunsOn's license (`runs-on/key`:
a yearly fee, no per-minute one). A full run's legs come to about $0.60. The account's monthly budget (`budget_monthly_usd`, 300) emails
`alerts/email` at 80% spent and at a 100% forecast, RunsOn emails a weekly cost report
there, and a job hung past 90 minutes is ended with its instance.

Once, in order:

1. **[admin] The account**, as the deployments' were (section 3, step 2): `aws
   organizations create-account --email aws-ci@<domain> --account-name opennova-ci`, its
   id, name and root address into `aws-ci` (`secrets init` makes the item's placeholders;
   `infra/zone` forwards `aws-ci@<domain>`). `org apply` adopts it (no state bucket and no
   deploy user there: the ci stack runs as `opennova-org`) and gives your Identity Center
   user both permission sets in it; `zone apply` makes its root address's route.
2. **[admin] `opennova-org`'s policy** names the account's `OrganizationAccountAccessRole`:
   apply `infra/aws/iam/opennova-org.json` filled again (`infra/aws/iam/README.md`; the org
   stack's output `org_user_policy` prints it filled).
3. **The license**: RunsOn's license key (runs-on.com) into the vault item `runs-on`, field
   `key` (the toolbox finds the item's title in any case).
4. **[admin] The account's EC2 quotas** (the access portal, `OpenNovaAdmin` in
   `opennova-ci`, Service Quotas): a run takes 96 vCPUs at once (six 16-vCPU legs). The
   quotas are the runners' spending ceiling: "All Standard (A, C, D, H, I, M, R, T, Z) Spot
   Instance Requests" (L-34B43A08) and "Running On-Demand Standard (A, C, D, H, I, M, R,
   T, Z) instances" (L-1216C47A, for when spot is short), 192 each in OpenNova's account:
   two runs at once. A leg past them waits for an earlier run's instances to end, or
   takes on-demand capacity when spot alone is full. A new account's can be lower than
   one run (OpenNova's spot quota started at 96): check both.
5. **The stack:**

   ```bash
   ./deploy/run.sh ci plan
   ./deploy/run.sh ci apply                     # type ci
   ./deploy/run.sh ci output setup_url
   ```

   Confirm the subscription RunsOn's alert topic emails to `alerts/email`.
6. **The GitHub App**: open `setup_url`, **Register GitHub App** for `opennova-net` (as an
   organization owner), and install it on the `opennova` repository alone. Its credentials
   stay in the ci account (Secrets Manager); nothing goes in the vault.
7. **[admin] Outside contributors' runs wait for approval**: a fork's pull request runs its
   own copy of `ci.yml`, which could name RunsOn's labels, so the organization and the
   repository require an admin's approval before any workflow of an outside contributor
   runs (Settings, Actions, "Approval for running fork pull request workflows from
   contributors": **Require approval for all external contributors**). The GitHub provider
   has no resource for it, so `infra/github` leaves it alone:

   ```bash
   gh api -X PUT orgs/opennova-net/actions/permissions/fork-pr-contributor-approval \
     -f approval_policy=all_external_contributors
   gh api -X PUT repos/opennova-net/opennova/actions/permissions/fork-pr-contributor-approval \
     -f approval_policy=all_external_contributors
   ```

   Read a fork's workflow changes before approving its run.

A leg that sits queued: RunsOn's alert email, the account's quotas (a launch past them
fails), or the GitHub App's installation. The labels (`cpu=16`, the instance families, the
image `windows25-full-x64` or `ubuntu24-full-x64`, the volume) are `ci.yml`'s, on each leg.
RunsOn's Windows image carries Visual Studio 2022 (GitHub's `windows-latest` and the
maintainers build with 2026), and its Ubuntu image leaves Ninja out (the Linux legs install
it). A newer RunsOn: the module's `version` in `infra/ci/runners.tf`, then `ci apply`.

## Operating

Every command here takes the deployment it reads or changes: `--deployment staging` or
`--deployment prod`.

- **At a glance.** The site's admin page **Infrastructure** (`/admin/infrastructure`) shows
  what `app status` does and more: each program's service (its counts, rollout and revision
  against the release), its tasks (health, zone, address, image, the recently stopped ones'
  exit codes) and events, the load balancer's targets, the database's and the cache's status,
  the lobby instances, the fleet leader and the machines. Web reads AWS for it with its task
  role (read-only, `infra/aws/programs.tf`), at most every 20 s. Each program's card opens its
  usage and its logs, and the database, the cache and the load balancer open their CloudWatch
  charts. **Fleet** (`/admin/fleet`) starts with what needs you (the fleet's findings and the
  firing alarms, counted in red on the admin menu), then a card per machine opening its page
  (its facts against the release, trees, timeline, usage and hostd's log).
- **Logs.** `./deploy/run.sh --deployment <deployment> app logs [PROGRAM|backup] [--follow] [--since 2h]` reads
  CloudWatch (`/opennova/<deployment>/service`, 30 days, a stream per container and task,
  `<program>/<container>/<task id>`: web's task writes `web/web/...` and
  `web/portal/...`; a program's name tails all of its tasks'). The game machines' are
  `fleet logs` ("Hosted game servers", Operating notes). The site's admin pages read the same
  groups (`GET /api/admin/logs`: a program's tasks, a machine's hostd, a hosted server's
  starts) with web's task role, which reads their events alone (`infra_read`), and each
  program's, machine's and server's CPU, memory and network from the rows the service writes
  (`GET /api/admin/usage`, `GET /api/admin/programs`), the database's, the cache's and the
  balancer's from CloudWatch's metrics (`cloudwatch:GetMetricData`, kept 60 s).
- **Alarms** go to the deployment's SNS topic, `opennova-<deployment>-alerts` in its account, which emails
  `alerts/email` (confirm the subscription from the first email AWS sends): per program,
  no heartbeat for 5 minutes, more boots in 15 minutes than its `max` and 3 (a crash
  loop), fewer tasks running than its `min` for 5 minutes (these two once the programs
  have a release: before the first `app deploy` no task runs, and silence is no alarm),
  and its CPU or its memory over 80% for 15 minutes; a load balancer target group (the gate, the lobby, 443, the retail port, the update service) with
  an unhealthy target for 10 minutes; a failed hosted server, a lost machine, 3 failed
  launches in 15 minutes, a billing sync older than an hour, 5 refused webhooks in 15
  minutes, the entitlement stops held for 5 minutes (`limits_held`: the supervisor's
  mass-stop guard keeps over-entitled servers running until billing is trusted again;
  the admin billing view says why), a gate probe left unanswered in 5 minutes
  (`probes_unanswered`: the gate had no lobby to hand out, so no client can log in while
  every health check passes; `LobbiesLive` and `app logs lobby` say why); a machine stuck
  in a state (launching or booting past 20 minutes, draining past 60, ending past 15, lost
  past 10, or moving more than 6 times in an hour: the site's Attention list names it), a
  machine's disk under 15 % free for 10 minutes, the servers' log group taking in more than
  `log_ingest_alarm_gib_per_day` (2) GiB a day's rate in an hour (a server logging in a
  loop), and with hosting on and the fleet released, no fleet leader whose last clean pass
  is under 2 minutes old for 10 minutes (no data included: no leader logs it); the database's CPU over 80% for 15 minutes, its CPU
  credit balance low (a burstable class), its connections over 80% of `max_connections`
  and its free storage; a cache node evicting keys or short of CPU credits; and the Route
  53 health check on `http://<the gate's address>:8080/healthz`, the way a stock client
  comes. A task failing its health check is its service's to replace. The heartbeat is
  each process's JSON line every 60 s, `{"heartbeat":{"program":"...",...}}`; its fields
  are metrics in `OpenNova/<deployment>`, by `Program`, and each boot line,
  `{"boot":{"program":"..."}}`, counts a boot.
- **Budgets**, each deployment's in its own account, counting that account (every cost
  in it is the deployment's): the month's cost (`budget_monthly_usd`: prod 250, staging
  200) and the EC2 data transfer out (`budget_egress_gb`): an email at 80% and at a 100%
  forecast; Cost Anomaly Detection by AWS service emails daily anomalies over
  `cost_anomaly_threshold_usd`. The hosted servers' egress is metered and shown, never
  capped.
- **A shell.** `./deploy/run.sh --deployment <deployment> app shell PROGRAM [CONTAINER]` (ECS Exec; the toolbox
  carries the Session Manager plugin it uses). `db psql` opens psql as the
  service's role in a one-off task over ECS Exec; `db psql -c 'SELECT ...'` runs one
  statement in a one-off task and prints its output.
- **New tasks.** The tasks keep no state: `app restart PROGRAM` replaces a
  program's tasks with new ones of the same revision, the way a deploy does. Scaling a
  program is its var-file sizes and `infra apply` (its `min` and `max`; a new `cpu` or
  `memory` makes a new template, which reaches the tasks at the next `app converge`).
- **Changing a secret.** Edit the deployment's vault item, `secrets sync`, `app converge`. A new
  `database_password` takes `db bootstrap` (it sets the role's password) between the sync
  and the converge.

### Cost

About $160 a month for each deployment's service tier before the machines (approximate:
us-east-1 on-demand list prices, rounded: Fargate Linux x86 $0.04048 a vCPU-hour and
$0.004445 a GB-hour, a public IPv4 address $0.005 an hour, a network load balancer
$0.0225 an hour and its LCUs, `db.t4g.micro` single-AZ $0.016 an hour, RDS gp3 $0.115 a
GB-month, `cache.t4g.micro` Valkey $0.0128 an hour, 730 hours a month):

| | a month |
|---|---|
| the gate, a 0.25 vCPU / 0.5 GiB task and its public IPv4 | $13 |
| the lobby, the same | $13 |
| web, a 0.5 vCPU / 1 GiB task (the server and nginx) and its public IPv4 | $22 |
| the fleet program, a 0.25 vCPU / 0.5 GiB task and its public IPv4 | $13 |
| the worker, a 0.5 vCPU / 1 GiB task and its public IPv4 | $22 |
| the data program, a 0.25 vCPU / 1 GiB task, 20 GiB of task storage past Fargate's 20 and its public IPv4 | $16 |
| the load balancer, its LCUs at this traffic and its two Elastic IPs | $25 |
| the database, `db.t4g.micro` in one zone, 20 GiB gp3, backups | $14 |
| the cache, one `cache.t4g.micro` Valkey node | $9 |
| CloudWatch logs, metrics, about 50 alarms; the health check; Cloud Map's zone | $13 |
| S3 (releases, dumps), SSM, the nightly backup task, the schedule | about $1 |

Each further web or worker task adds about $22, a further lobby about $13; Multi-AZ
doubles the database's $14, and each further cache node adds $9. Staging has prod's shape
(about $160) and costs about $70 while `app down` (the database, the cache, the load
balancer and its addresses stay). A deployment's extra tasks (every
program's new tasks start beside the old) run for minutes. The database adds its surplus CPU credits, if any (unlimited mode: about $0.075 a
vCPU-hour past its baseline over a day), none while it stays under its baseline. The
monthly budget of prod's account (`budget_monthly_usd`, 250) leaves about $90 for the
machines and their egress; staging's account has its own (200).

The machines: a full shared machine (`t3a.medium`, its address and volume, about $34) is
about $5.70 a server at 6 servers, before egress (its CPU credits add a little under a
sustained load); an own machine (`c7a.medium`) about $43. With static addresses sold, the
play zone adds $0.50, and each address $3.65 while no server runs at it (net $0 while one
does: it replaces the machine's own public IPv4).
Egress is about $0.09 a GB, and a full 64-player server sends about 1.8 GB an hour, so
egress can cost more than the machine: the egress budget is the alarm. The first shared
machine runs at a loss until it fills.

## Smoke tests

On the deployment the tests name (`--deployment staging` or `--deployment prod` on
every command).

- **A. The stack's front.** `curl -I https://<domain>/` answers 200; `curl -i
  http://<the gate's address>:8080/healthz` answers 200 `ok` (and `/livez` too); `curl -i
  http://<the gate's address>:8080/jop_2.gsb` answers 200; `curl -i http://<the gate's
  address>:8080/api/health` answers 403 (`proxy_required`); `curl -m 5 http://<the gate's
  address>/` times out (no port 80); `curl https://<site host>/api/server-info` names the
  gate's Elastic IP, and so does `nslookup nw.<domain>`; `./deploy/run.sh --deployment <deployment> aws ec2 describe-addresses
  --allocation-ids <id>` shows it on the load balancer's zone-a interface.
- **B. Operations.** `app status` shows every program's service at its count, its
  rollout COMPLETED on the release's revision; `app logs` shows a stream per container and
  task; the `Heartbeat` metric has points for each `Program`; the SNS email subscription
  is confirmed; the Route 53 health check is green; the load balancer's five target
  groups (the gate's, the lobby's, 443, the retail port's, the update service's) are
  healthy (EC2, Target groups); `app shell fleet` opens a shell; `backup now` writes
  a dump.
- **C. Accounts.** Register, log in, log out; the admin pages open.
- **D. Billing.** Stripe's "Send test webhook" answers 2xx; subscribe to Tier 1 ($5, test
  card in test mode) or give a comp grant; the account's entitlement shows it.
- **E. Retail.** A stock JO client signs in through the gate and lists the lobby (at the
  gate's address, port 64206; `app logs lobby` names its session); its HTTP requests
  reach the gate's address on 8080. With web at two tasks or more, repeat the login until
  its steps land on both (web's logs show it): it completes.
- **F. A shared machine.** With a data set uploaded on Game data, start a server from My
  servers: its timeline runs requested, provisioning (a shared machine launching from the
  pinned AMI and joining the fleet cluster, its hostd task starting there, `fleet status`,
  hostd reaching the fleet program by its Cloud Map name and fetching the server's trees
  from the store: a few minutes the first time), booting,
  listing, running. It lands on slot k, lists in its game's lobby in the OpenNova and the
  retail server browsers, and is joinable by a retail and an OpenNova client; the site's
  console works (players, chat, a map change) and the server's stats move; `fleet logs
  server <id>` shows the server's output, `fleet logs machine <machine>` the agent's, and
  `fleet status` lists nothing that needs you.
- **G. A second account** starts one: it lands on slot k+1 of the same machine.
- **H. Deploys that keep the lobby.** With a server listed and a retail client in the
  lobby: `app deploy --image portal=<tag>` (nothing restarts but web's task), then `app
  restart web`, `app restart worker`, `app restart data`, `app restart fleet` and `app restart gate`:
  the client stays in the lobby and the server stays listed through each.
- **I. A lobby's replacement.** On staging, with a hosted server listed and a retail
  client hosting its own game: `app restart lobby`: the new lobby task starts beside the
  old and takes the balancer's new flows once it passes; the old one is drained (10 s)
  and stopped. Its players sign in again through the gate onto the one lobby address.
  The hosted server and the retail self-host both list again on the new task within about
  5 minutes (a host's session times out after 240 s and reconnects to the same address)
  with no restart: the server's timeline shows no `placed again`, its match goes on, and
  it is never `failed`. A lobby
  image that never turns healthy (`app deploy --image lobby=<a broken tag>`) leaves the
  old lobby serving while the circuit breaker rolls it back, and the deploy records
  nothing.
- **J. Stop** a server: its slot frees; stop the last: the machine ends after the idle
  time. **Stop all** (the kill switch) stops everything and the machines end.
- **K. Alarms.** On staging: `app shell lobby`, then `kill -STOP <the program's pid>`
  (`ps`): its container health check fails and ECS replaces the task, and the lobby's
  heartbeat alarm emails if no heartbeat came for 5 minutes; the same in a web task turns
  its targets unhealthy before ECS replaces it.
