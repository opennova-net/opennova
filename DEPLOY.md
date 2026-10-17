# Deploying your own OpenNova NovaWorld instance

This repository deploys a complete NovaWorld stack to AWS, behind Cloudflare: the gate,
the lobby, the legacy HTTP services, the web portal, the fleet's controller and its
background jobs, a PostgreSQL database, a Valkey cache, and the fleet of machines the
hosted game servers run on. Anyone can stand up their own instance with the commands
the project maintainers use. The original games reach your server through a
hosts-file line that points `gs.novaworld.net` at its gate's Elastic IP (the OpenNova
Switcher writes it; DEVELOPING.md, "Test with retail Joint Operations").

The stack in short: the NovaWorld service is five programs, each its own image and an ECS
service on Fargate across two availability zones, whose tasks keep no state of their own:

| Program | Runs | Tasks (default) | Reached at |
|---|---|---|---|
| gate | the gate (UDP 7597; the hosts' status blobs land there too) | 1, 0.25 vCPU / 0.5 GiB | the public network load balancer, at the gate's Elastic IP |
| lobby | a lobby instance (UDP 64206) | 1, 0.25 vCPU / 0.5 GiB | the public network load balancer, at the gate's Elastic IP: one lobby address for every lobby task, which the gate hands out |
| web | the retail HTTP routes and the website's API (8080) and the portal's nginx (443) | 1 to 2 on its CPU, 0.5 vCPU / 1 GiB | the load balancer: 443 from Cloudflare, 8080 from everyone |
| controller | the fleet's reconcile pass, job scheduling, the machines' internal listener (8070) | 1 (elected; a standby may wait beside it), 0.25 vCPU / 0.5 GiB | `controller.<deployment>.opennova.internal`, its Cloud Map name |
| worker | the background jobs (the fleet's AWS calls, billing sync, mail, prunes) | 1 to 2 on its CPU, 0.5 vCPU / 1 GiB | nothing inbound |

Its database is RDS PostgreSQL 17 in private subnets, Multi-AZ on a small burstable
class; web's sessions and rate limits live in ElastiCache for Valkey beside it; its
secrets live in SSM Parameter Store, copied from a 1Password vault, and ECS hands each task
its own; a release is each program's task definition revision with its images pinned by
digest; its logs go to CloudWatch; the nightly database dump is a scheduled task. No SSH
anywhere: a shell is ECS Exec. The gate's Elastic IP is one no Terraform plan can release,
the load balancer's address in its first zone, where retail clients reach the gate, the
lobby and the retail HTTP routes; the website reaches web through Cloudflare alone, over
TLS Cloudflare verifies. Hosted game servers run on machines the service launches itself:
shared machines holding up to 12 servers, and own machines (a paid add-on) holding one.

## Deployments: dev, staging and prod

There are three places the service runs, and nothing of one reaches another by accident:

| | dev | staging | prod |
|---|---|---|---|
| What it is | a developer's local stack: the five programs, the portal, PostgreSQL and Valkey under docker compose (`deploy/compose`, DEVELOPING.md) | the rehearsal: prod's shape (two web tasks), every change and every cutover runs here first | the service players use |
| Where | the developer's machine | its own AWS account, `opennova-staging`, in the Organization | its own AWS account, `opennova-prod` |
| Site | `http://localhost:5173` | `https://staging.<domain>` | `https://<domain>` (and `www`) |
| Retail gate | the developer's LAN address | its own Elastic IP | the address retail hosts files name (`gs.novaworld.net`), transferred into the prod account |
| AWS credentials | none | its deploy user's key (vault `aws-staging`), or its GitHub environment's OIDC role | its deploy user's key (vault `aws-prod`), or its GitHub environment's OIDC role |
| Stripe | test mode, the restricted key (`stripe-test/restricted_key`) | test mode (`stripe-test`) | live mode (`stripe-live`) |
| Mail | none (no AWS credentials: logged as not sent) | its own SES identity, `staging.<domain>` (off unless `mail_sender` is set) | its own SES identity, `<domain>`, `support@<domain>` |

**Separate, per deployment.** Everything AWS: each deployment is an account of its own
(the Organization's management account holds no OpenNova resource), so its VPC, cluster,
database, cache, buckets, SSM parameters, logs, alarms and their topic, budgets, SES
identities and sandbox, Cloud Map namespace, hosted machines and data snapshots, and its
Terraform state (a versioned, encrypted bucket in that account, locked by S3's own lock
file) are its own, and no IAM principal of one account can touch the other's. Each
deployment's credentials: its deploy user (`opennova-deploy` in its account; its key in
its own vault item) and its GitHub environment's OIDC role (which trusts that
environment's jobs alone). A command holds its own deployment's key alone, with two
exceptions, each one way and named by its flag: `backup import --from-deployment prod`
on staging reads prod's dump with prod's key, and `app deploy --promote staging` on prod
reads staging's release with staging's key; each in a subshell of its own, the two keys
never in one call. Its vault items
(`aws-<deployment>`, `app-<deployment>`, `rds-<deployment>`, `hosted-<deployment>`), its
Stripe mode, its site's hostname, its origin certificate (staging's names
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

## Rebuilding from the vault

Everything a rebuild needs that Terraform cannot make lives in the 1Password vault; the
repository holds `op://` references, never a value. A rebuild is: restore the vault, run
the manual steps below (each fills the vault field it names), then the applies
(sections 3 and 4; moving an existing stack, "Moving prod into its account").

| Item | Field | Filled by |
|---|---|---|
| `aws-org` | `account_id`, `organization_id`, `region` | hand: the management account, its Organization (section "The Organization and the accounts") |
| `aws-org` | `access_key_id`, `secret_access_key` | hand: the `opennova-org` user's key (`infra/aws/iam/README.md`'s commands write it) |
| `aws-org` | `sso_given_name`, `sso_family_name`, `sso_email` | hand: the maintainer's IAM Identity Center user |
| `aws-org` | `sso_portal_url` | `./deploy/run.sh org apply` |
| `aws-prod`, `aws-staging` | `account_id`, `account_name`, `root_email`, `region` | hand: the member account (`secrets init` makes the placeholders) |
| `aws-prod`, `aws-staging` | `state_bucket`, `access_key_id`, `secret_access_key`, `github_deploy_role_arn` | `./deploy/run.sh org apply` |
| `aws-prod` | `gate_eip_allocation_id` | hand: the gate address's transfer into the prod account (accept-address-transfer prints it) |
| `cloudflare` | `api_token`, `zone_id`, `domain` | hand (section 1) |
| `app-prod`, `app-staging` | `admin_api_token`, `admin_basic_auth_user`, `admin_basic_auth_password`, `database_password`, `redis_password`, `edge_auth_secret` | `secrets init` |
| `rds-prod`, `rds-staging` | `master_password` | `secrets init` |
| `hosted-prod`, `hosted-staging` | `data_snapshot_id_<slug>` | `hosted data-snapshot` prints each; hand |
| `stripe-test`, `stripe-live` | `secret_key` (and `restricted_key` in `stripe-test`) | hand (Stripe's dashboard) |
| `stripe-test`, `stripe-live` | `webhook_secret`, `price_*` | `stripe setup` |
| `alerts`, `email` | `email`, `forward_to` | hand |
| `github-terraform`, `reference-assets` | (section "GitHub") | hand |

The manual steps outside the vault: the Organization and its member accounts, the
`opennova-org` user's policy, each account's SES production access and service quotas
(below), the root addresses' Email Routing verification click, the GHCR packages' public
visibility, the Stripe dashboard's settings, and the gate address's transfer into the prod
account (the move: "Moving prod into its account").

## What you need

- **Docker** on the machine you deploy from. That is the only dependency: the deploy
  toolbox (1Password CLI, Terraform, the AWS CLI, the docker client) runs in a container.
- **A 1Password account** (any paid plan has service accounts) with a vault for this
  deployment.
- **An AWS Organization** with two member accounts, one per deployment (below).
- **A Cloudflare-managed domain.** The website is served through Cloudflare's proxy only:
  the origin admits Cloudflare's address ranges alone and presents a Cloudflare Origin CA
  certificate.
- **A Stripe account** for the monthly support tiers (the paid hosting). Without one the
  stack runs with billing off: nobody can subscribe, and only an admin's comp grant lets
  an account host servers.

No secret is ever committed to the repository or written to your disk outside a
container's in-memory tmpfs, except what the Terraform states hold (each deployment's
deploy user key in the org stack's, the origin certificate's key in each deployment's;
they live in versioned, encrypted buckets the deploy users alone reach). The only secret
you handle directly is a 1Password service-account token.

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
| `app-<deployment>` | `admin_api_token`, `admin_basic_auth_user`, `admin_basic_auth_password`, `database_password`, `redis_password`, `edge_auth_secret` | random values (`admin` for the user) |
| `rds-<deployment>` | `master_password` | random |
| `aws-<deployment>` | `account_id`, `root_email` | `REPLACE_ME` |
| `aws-<deployment>` | `account_name`, `region` | `opennova-<deployment>`, `us-east-1` |
| `aws-org` | `account_id`, `organization_id` | `REPLACE_ME` |
| `aws-org` | `region` | `us-east-1` |
| `stripe-test`, `stripe-live` | `secret_key`, `webhook_secret`, `price_tier1`, `price_tier2`, `price_tier3`, `price_own_machine` | `REPLACE_ME` |
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
`app-<deployment>/edge_auth_secret_previous` (a rotation). Item `hosted-<deployment>` holds
that deployment's hosted servers' data snapshots (section 7). There is no SSH item.

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
./deploy/run.sh --deployment staging secrets check              # every reference, the Stripe items
```

`secrets check` resolves every reference without printing a value, and reports each
Stripe item as `OK (test mode, 3 tiers + own machine)`, `placeholders: <fields> (billing
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
   `infra/zone` makes their rules, step 5):

   ```bash
   aws organizations create-account --email aws-prod@<domain> --account-name opennova-prod
   aws organizations create-account --email aws-staging@<domain> --account-name opennova-staging
   aws organizations list-create-account-status --states SUCCEEDED   # each new account's id
   ```

   Each account's id goes in `aws-<deployment>/account_id`, its name in `account_name`,
   its root address in `root_email` (`./deploy/run.sh --deployment <deployment> secrets
   init` makes the fields). Each account has the role `OrganizationAccountAccessRole`,
   which the management account may assume. Its root user gets a password once its
   address receives mail (step 5).
3. **[admin] The `opennova-org` user**, the management account's one OpenNova principal:
   `infra/aws/iam/opennova-org.json` filled with the four ids (the Organizations reads,
   account creation and tagging, Identity Center's user and assignments, and assuming the
   two member accounts' `OrganizationAccountAccessRole`, nothing else). The commands
   (`infra/aws/iam/README.md`) make the user and put its key straight into
   `aws-org/access_key_id` and `secret_access_key`.
4. **The org stack:**

   ```bash
   ./deploy/run.sh org plan
   ./deploy/run.sh org apply                    # type org
   ```

   `infra/org` adopts the Organization and both accounts, and in each account through
   its `OrganizationAccountAccessRole` makes: the Terraform state bucket
   (`opennova-tfstate-<account id>`, versioned, encrypted, TLS alone, reached by the
   account's deploy user and admin role alone), the account's S3 public access block, the
   role boundary every role a deployment's stack makes must carry, the deploy user
   `opennova-deploy` with the deploy policies (`infra/aws/iam/opennova-deploy-*.json`,
   filled for the account) and its access key, and the GitHub deploy role (trusting the
   repository's environment of the same name alone); in the management account, the
   Identity Center user and its two permission sets (`OpenNovaAdmin`, `OpenNovaReadOnly`)
   in both accounts. Its first run applies with a local state, then moves the state into
   prod's state bucket (`org/terraform.tfstate`), where it lives. Every apply writes back
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
   their variables). Moving an existing stack out of the management account, the zone's
   settings and the organization's state are adopted first ("Moving prod into its
   account", section 1).

   [admin] Then, the root addresses' routes live and Cloudflare's verification link
   clicked (section 4, "Email"): give each member account's root user a password (the
   sign-in page's "Forgot password" mails the root address) and an MFA device, then leave
   it unused.
6. **[admin] Per account, in its console** (the access portal, `aws-org/sso_portal_url`,
   with `OpenNovaAdmin`): Amazon SES, **Request production access** (prod at least;
   "Outbound mail (SES)"); Service Quotas: the Fargate On-Demand vCPU quota (L-3032A538,
   about 10 at the defaults' maxima with a deployment's extra tasks) and the EC2
   On-Demand Standard quota (L-1216C47A, 32 for the machines: section 7); Billing, Cost
   Explorer once (a member account's budgets read its own costs).

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
names it in the prod account (the cutover transfers it there). Terraform reads that
address and tags it, and never manages it: no plan can release it. It is the public load
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
the load balancer, the ECS cluster or a program's service, the controller's Cloud Map
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
health port from the load balancer; web: 443 and 8080 from the load balancer alone; the
controller: the internal port from the machines alone; the worker: nothing; no 22, no 80
anywhere), a task role holding what
that program needs alone, a task definition (the template every deploy clones), an ECS
service on Fargate in both public subnets with a public IPv4 per task, made with no task
(nothing runs before the first release, below), and its task counts (Application Auto
Scaling between the program's `min` and `max`, web and the worker on their CPU); the task
execution role (the deployment's secrets and the log group); the controller's Cloud Map
name; the public network load balancer with the gate's Elastic IP (adopted on prod) in
zone a and one of its own in zone b, carrying the gate, the lobby and web, and its own
security group; the database and the cache (Valkey, a primary and a replica); the backup task and its nightly schedule; the
SSM parameters the toolbox reads; the operations bucket (`opennova-<deployment>-ops-<account
id>`: the releases and the dumps); the logs, metrics and alarms; the account's budgets;
the deployment's Cloudflare records (its site's, proxied), its origin certificate; its
outbound mail's SES identities and records (with a sender); and (with data snapshots) the
fleet's launch templates and image parameter. A new database takes 10 to 15 minutes, a new
cache about as long. The services run no task until the deployment's first release: a
template's images are unpinned (`image_tag`, `latest`), so no task starts from one, and
each count's floor is 0 until then; the first `app deploy` starts each program at its
`desired` count and raises its floor to its `min`, which the next `infra apply` keeps
(section 5).

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

The instance is `db.t4g.small` (`db_instance_class`: 2 vCPUs, 2 GiB), a burstable class
RDS runs in unlimited mode: a load past its baseline spends surplus CPU credits, billed
by the vCPU-hour, and is never throttled (the `db-cpu-credits` alarm says when its
balance runs low: a larger class, or Performance Insights' top statements). Multi-AZ
(`db_multi_az`): a synchronous standby in the other private subnet's zone that RDS fails
over to on its own, the endpoint's name following it, in a minute or two.
`max_connections` is sized rather than left to RDS's memory formula (about 200 for
2 GiB): `db_pool_max` (7, the programs' `DATABASE_POOL_MAX`; at least 6, a worker's four
job threads, its keeper and its main loop) for each of `db_service_processes` (14)
program processes, plus 10 for the backup task, the operator's psql and the reserved
connections (108). The 14 are every program's tasks at its `max`, doubled for the new
tasks a deployment starts beside them: gate 1, lobby 1, web 2, controller 1, worker 2 (2
+ 2 + 4 + 2 + 4); the plan refuses a `max` past them (raise
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

`infra/aws/cache.tf` makes web's ephemeral store: ElastiCache for Valkey 8, a primary and
a replica in the two private subnets' zones (`cache_node_type`, default
`cache.t4g.micro`), failing over on its own; in-transit TLS required, encrypted at rest,
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
UDP port (64206, checked the same way) and web's retail HTTP port (8080, checked on
`/healthz`), all to IP targets that keep each client's own address and port, and 443
(the portal's nginx, from Cloudflare's ranges alone; with proxied records only, since
without them there is no origin certificate and nginx listens on 80). Deletion
protection is on: the balancer holds the gate's address.
`ONNET_PUBLIC_HOST` (the gate's `POSTIPADDRESS`) and `ONNET_WEB_HOST` (the startup URL,
the web domain, `HOST_URL`, `GSB_SERVER`) both name that address, so no retail client
follows a DNS name; the site's records point at the balancer's DNS name.

Every lobby task announces that address with the lobby's port (`ONNET_LOBBY_ENDPOINT`,
`<the gate's address>:64206`), each its own lobby instance behind it, and the gates hand
that one address out. The balancer spreads the clients across the lobby tasks by flow (a
client's address and port) and keeps each flow on one task; a lobby sees each host's own
address and port, which its listing's join target is. A stock host's NovaWorld session
reconnects only to the lobby address the gate gave it at its first connect and drops a
silent lobby after 240 s, so a host whose lobby task went away reconnects to the same
address and lands on a live task. A lobby task leaving (a deploy, a restart) is
deregistered first: the balancer sends it no new flow, keeps its flows 10 s, then ends
them, and ECS stops it only then. The controller's tasks register in Cloud Map as
`controller.<deployment>.opennova.internal` (an A record, TTL 10, in a zone private to
the VPC); the leader alone binds the internal listener, on its task's private address,
where the machines' hostd and `ONNET_INTERNAL_URL` reach it, so a new controller in
either zone is found within the record's TTL.

Every task answers `/livez` (its program's stall watchdog) on the health port (web on 8080
as well), which its container health check asks; ECS replaces a task that fails it, and a
deployment counts a new task only once it passes (and, behind the balancer, once the
balancer's check does). Web's `/healthz` also fails while the cache does not answer: the
balancer routes around it, and no task is replaced for it, since `/livez` is what ECS
asks.

Each program's sizes are a var-file setting (`gate`, `lobby`, `web`, `controller`,
`worker`: a `cpu` and `memory` of a Fargate size, `min`, `max` and `desired`, any left
out keeping its default). `desired` is the count a new service starts with; Application
Auto Scaling holds each count between `min` and `max`, web's and the worker's on their
CPU (60% on average), the others where they start. The programs' maxima, with a
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
plain HTTP to it at the gate's address, which the gate announces in its startup URL
(`ONNET_WEB_HOST`), and `ONNET_API_PROXY_ONLY=1` refuses its `/api/` to anyone but the
portal's nginx, which reaches web's server on the task's loopback
(`ONNET_TRUSTED_PROXIES=127.0.0.1`). The balancer hands each connection to a web task
from the client's own address, as the retail routes and the rate limits need.

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
`-lobby`, `-web`, `-controller` and `-worker` (one Dockerfile, a target each); the
portal's nginx, `novaworld-web`; the backup task's, `opennova-novaworld-backup`
(`deploy/backup`); and `opennova-serve` (the hosted servers, `serve-image.yml`; it also
carries hostd, the machines' agent). Make each package public once: the tasks and the
machines of both accounts pull anonymously (the task execution role holds no registry
credentials).

```bash
./deploy/run.sh --deployment staging app deploy --tag sha-abc1234 --serve-tag sha-def5678
./deploy/run.sh --deployment prod app deploy --promote staging      # type prod
```

`app deploy`:

1. resolves each image to its digest now: `--tag` sets the five programs', the portal's
   and the backup task's (all built from one commit), `--image NAME=TAG` one of them
   (`gate`, `lobby`, `web`, `controller`, `worker`, `portal`, `backup`, `serve`, `hostd`;
   `NAME=@sha256:<digest>` pins a digest as it is), `--serve-tag` and `--hostd-tag` the
   machines'. An image given no tag keeps the current release's digest; the first release
   takes `latest` for any tag not given. `--promote staging` (prod alone) takes, instead,
   exactly the images staging's current release runs, by digest, read in staging's
   account with staging's own credentials (`app images` prints them; `--images-from
   FILE|-` takes the same JSON, the deploy workflow's way), and records which release it
   promoted;
2. registers each program's task definition revision: Terraform's task definition (the
   template, `infra.env` names it) with the release's images, the serve and hostd images
   in the environment of the programs that read hosting (web, the controller, the worker;
   `--no-hosting` turns their provider off), every secret the template names (SSM must
   hold each) and each optional one its label names that SSM holds. A program whose
   revision would run exactly what it runs now keeps it, so a release that changes only
   web's image restarts web alone and never a lobby session;
3. on prod, prints the images and asks (type `prod`, or `--yes`);
4. rolls the services in turn: worker, controller, web, lobby, then gate, the players' way
   in, last. It follows each service's new deployment until ECS calls it COMPLETED (its
   new tasks healthy, the old stopped) or FAILED: ECS's deployment circuit breaker rolls a
   deployment whose new tasks never turn healthy back to the old revision, after 10 failed
   launches, so the wait is bounded at 40 minutes (`OPENNOVA_ROLL_TIMEOUT`, seconds). A
   failed or unfinished one stops the deploy, leaving the rest on what they ran. A service
   at no task before the deployment's first release starts at its `desired` count, and
   its floor is raised to its `min`;
5. once every program runs it, registers the backup task's revision (the nightly run
   takes the newest) and records the release: its manifest (the images by digest, each
   revision, the previous release, the release it was promoted from) goes to
   `s3://<ops bucket>/releases/<id>.json` and to `/opennova/<deployment>/release`. A
   deploy that stops records nothing: the release it was on stays the deployment's, and
   `app rollback --to <that id>` puts the programs it already moved back on it.

How each program's tasks are replaced: every program starts the new task first and stops
the old one once it is healthy, so a bad image stops nothing that runs (the controller's
new task waits as the standby, then leads once the old one lets the lock go; the
balancer sends new lobby flows to the new lobby task once it passes, then drains the old
one for 10 s before ECS stops it). A lobby's restart is the one that shows: its players
sign in again through the gate, and the servers listed on it leave the browser for up to
about 4.5 min: a host's NovaWorld session (retail's, `opennova-serve`'s too) drops the
silent lobby after 240 s, reconnects to the one lobby address, lands on the new task,
re-verifies with CurrentlyHosting=1 and is listed again, with no restart and its match
going on. Retail players' own hosted games list again the same way. The supervisor's
8 min, after which it places a hosted server whose listing is gone again (no restart
counted), is the safety net for a server whose lobby address is gone.
The controller's and the workers' jobs wait out a restart in the database.

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
./deploy/run.sh $D app shell controller               # a root shell in a running task (ECS Exec)
./deploy/run.sh $D app shell web portal               # web's second container, the portal's nginx
./deploy/run.sh $D app down                           # every program to 0 tasks (the counts recorded)
./deploy/run.sh $D app up                             # the recorded counts back
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
deployment's account was built can be rolled back to, with one limit: a rollback across a
migration is the database's business (section 6). The nightly backup runs the current
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
(handing them the stack's task roles alone), point its cluster's services and its
backup schedule at them, and nothing else.

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
of `comp_servers`) starts servers from the site's My servers page, picking a data set:
the game folder the server mounts, named by the games slug of the lobby it lists in
(`jop_2_consumer`, `dfx2_consumer`, `opennova_0_consumer`). The server is always the
public `opennova-serve` image, started with `/game <title>`.

The service places each server on a machine it launches itself (its EC2 machine driver,
in the worker's jobs, with the worker's task role), in either public subnet:

- **shared machines**, `t3a.large` with unlimited CPU credits, holding up to 12 servers
  (`shared_slots`) of any accounts, packed best-fit; an idle one ends after 30 minutes
  (`fleet_idle_minutes`);
- **own machines**, `c7a.medium`, exactly one server: the own-machine add-on.

Slot k of a machine is game UDP `32768+k` (to players) and admin TCP `32800+k` (to the
controller alone); hostd, the agent on each machine, runs one container per slot and
reports to the controller's internal listener (`controller.<deployment>.opennova.internal:8070`,
its Cloud Map name, admitted from the machines alone). Every machine mounts every data set,
read-only. Machines are terminated, never stopped. No machine has an SSH key or database credentials; break-glass is Session
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

Each deployment's account makes its own snapshots, encrypted with that account's default
EBS key (`aws/ebs`), private to it (such a snapshot cannot be shared with another account;
moving one across would mean re-encrypting it under a customer-managed key first), and they
never go in git or an image. Each file system carries its data set's
label, by which the machine mounts it.

### Turning it on

1. **Make each data set's snapshot, once.** Mount its game folder into the toolbox:

   ```bash
   HOSTED_DATA_DIR="D:/SteamLibrary/steamapps/common/Joint Operations Combined Arms" \
     ./deploy/run.sh --deployment staging hosted data-snapshot --data-set jop_2_consumer
   ```

   (PowerShell: `$env:HOSTED_DATA_DIR = "<folder>"; .\deploy\run.ps1 --deployment staging
   hosted data-snapshot --data-set jop_2_consumer`.) It streams the files into a helper
   image (`deploy/hosted-data`) that writes an ext4 image straight into a new snapshot of
   the deployment's account through the EBS direct APIs, with the deployment's deploy
   user's credentials, and prints its id.

2. **Record them in the vault.** The deployment's item, `hosted-<deployment>`, one field
   per data set, `data_snapshot_id_<slug>`. With no such item the deployment's hosting is
   off; a broken item stops every `infra` run rather than plan hosting away.

3. **Check the vCPU quota.** The machines alone use the account's On-Demand Standard
   instances quota (L-1216C47A; the programs run on Fargate, whose quota is its own):
   each `t3a.large` 2, each `c7a.medium` 1, and 2 for the image builder while it bakes.
   The defaults need 8 (2 x 2 + 2 x 1 + 2), in each deployment's account. Request 32 in
   the Service Quotas console to leave room for a roll:

   ```bash
   ./deploy/run.sh --deployment staging aws service-quotas get-service-quota --service-code ec2 --quota-code L-1216C47A
   ```

4. **Plan, then apply.** The plan adds the two launch templates
   (`aws_launch_template.fleet["shared"]`, `["own"]`), the fleet's image parameter, the
   machines' security group and its rules, the machine role and the worker's fleet
   policy, and changes web's, the controller's and the worker's task definitions (the
   `ONNET_HOSTED_*` settings, among them `ONNET_HOSTED_DATA_SETS`: the data sets the
   machines mount, which are the ones the site offers; `app converge` takes them). A data set's default map
   rotation for new servers is `hosted_data_set_missions` in the deployment's tfvars
   (`{ jop_2_consumer = ["m01.bms", ...] }`); without one, an owner picks the missions.

5. **Deploy**, so the programs read them and pin the serve and hostd images:
   `./deploy/run.sh --deployment staging app deploy --serve-tag sha-<serve> --hostd-tag sha-<serve>`
   (on prod, the next `--promote staging`). The
   controller and the worker log `[boot] hosted servers: ec2 ...`.

6. **Bake the machines' image** (optional, recommended): `./deploy/run.sh --deployment
   <deployment> fleet bake-image` (below), so a machine boots with docker and the images already there.

7. **Subscribe, or grant.** An account hosts once it subscribes to a support tier, or an
   admin gives it a comp grant (`comp_servers`, the admin user page). Run the smoke test
   below.

### Fleet commands

```bash
D='--deployment staging'                        # or prod
./deploy/run.sh $D fleet status                # EC2's view, then the service's
./deploy/run.sh $D fleet drain <machine>       # no new server there; its servers move
./deploy/run.sh $D fleet end <machine>         # end it (its servers move first)
./deploy/run.sh $D fleet roll                  # move every server to new machines
./deploy/run.sh $D fleet bake-image            # a new machine image (below)
```

They call the service's admin API inside a web task over ECS Exec, from the task's
loopback, with the admin token the web container holds. `fleet roll` after a new machine image or a new hostd: servers move
without counting restarts. On prod `fleet end` and `fleet roll` ask first (type `prod`,
or `--yes`). A deployment's worker launches and ends its own account's machines alone:
the other account's are out of its reach.

### The smoke test

1. Start a server from My servers. Its timeline runs requested, provisioning (a machine
   launching: a few minutes the first time), booting, listing, running.
2. It lists in its game's lobby in the OpenNova and the retail server browsers, at the
   machine's address and its slot's port; a retail client and an OpenNova client both
   join. A second account's server lands on the next slot of the same shared machine and
   is joinable too.
3. The site's console works (players, chat, a map change), and the server's stats move.
4. `app restart controller`: the running servers keep running, their hostd finds the
   new controller by its Cloud Map name, and they list again.
5. Stop one: its slot frees; with the machine empty, it ends after the idle time.
6. The kill switch (below) stops them all.

### The kill switch

The admin fleet view's **stop all** stops every live server and the service ends the
machines. To keep hosting off across deploys, deploy with `app deploy --no-hosting`. If
the service is down, `fleet status` lists the machines and `./deploy/run.sh --deployment
<deployment> aws ec2 terminate-instances --instance-ids <machine> ...` ends them; the service ends any machine
it finds without a server at its next pass anyway. To turn hosting off for good: stop all,
deploy with `--no-hosting`, delete the `hosted-<deployment>` vault item, `infra apply`.

### Operating notes

- **A machine image.** The launch templates launch from the image the SSM parameter
  `/opennova/<deployment>/fleet/image` names (`resolve:ssm:`), which Terraform seeds with
  Canonical's Ubuntu 24.04 and then leaves alone. `./deploy/run.sh --deployment <deployment> fleet bake-image`
  boots a one-off builder (a `t3a.small` with no instance role) from Canonical's newest
  image with `deploy/host/fleet-image.sh`, which does a machine's install stage (docker,
  held) and pulls the current release's serve and hostd images, then powers off; the
  toolbox images it (tagged `opennova:fleet-image`), points the parameter at it, ends the
  builder, and keeps this deployment's newest two images (the one before stays to point
  back at: `./deploy/run.sh --deployment <deployment> aws ssm put-parameter --name
  /opennova/<deployment>/fleet/image --type String --data-type aws:ec2:image --overwrite
  --value <ami>`). No plan changes; new machines
  boot from it, and `fleet roll` moves running servers onto new ones. Bake again after an
  `app deploy --serve-tag` (the pulled images go stale) and now and then for OS updates.
  A failed bake leaves the parameter alone and says how to read the builder's console.
- **New data, or a new data set.** Make its snapshot, set its vault field
  (`hosted-<deployment>`), `infra apply` (the templates mount it), `fleet roll`, then
  delete the old snapshot; staging first, then prod's own.
- **A machine's logs.** CloudWatch `/opennova/<deployment>/machines`, and the serial
  console (`./deploy/run.sh --deployment <deployment> aws ec2 get-console-output --instance-id <machine> --latest`) while EC2 still
  lists it.
- **A refused AWS call.** The service logs each failure (`app logs worker`); for an
  `UnauthorizedOperation`, `./deploy/run.sh --deployment <deployment> aws sts decode-authorization-message
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
./deploy/run.sh --deployment staging stripe check   # read alone: the item and its objects
./deploy/run.sh --deployment staging stripe setup   # make what is missing (idempotent)
./deploy/run.sh --deployment prod stripe check
```

The mode is the deployment's: prod live (`stripe-live`), staging test (`stripe-test`),
each on its own site; no command crosses them. On prod `stripe setup` asks first. `stripe setup` makes what the mode lacks and leaves
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
   support email `support@opennova.net` (section 4, "Email"), the website
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

Then `./deploy/run.sh --deployment <deployment> secrets check` (the item reads `OK`),
`secrets sync`, `app converge`: the server logs billing on; with an incomplete set it logs "billing off".
**Check delivery**: the endpoint's "Send test webhook" must answer 2xx. A challenge page
means Cloudflare stopped it: the webhook's custom rule skips the WAF's rules
(`infra/zone/edge.tf`), but Bot Fight Mode cannot be skipped by a rule, so keep it off
(section 1). On a plan without some skippable phase or product the apply names it: drop
it from `infra/zone`'s `cloudflare_webhook_skip_phases` / `_products`.

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

The stack is the organization's, not a deployment's: the toolbox runs it without
`--deployment`, and its state lives in prod's state bucket (`infra/github/terraform.tfstate`,
reached as prod's deploy user). It also makes the repository's two deploy environments,
`staging` and `prod` (prod's with the organization's admins as required reviewers and
protected branches alone), and their variables (`AWS_DEPLOY_ROLE_ARN`, `AWS_REGION`,
`AWS_ACCOUNT_ID`) from the vault's `aws-<deployment>` items (section 5, "The deploy
workflow"). What stays out of this public repository comes from the vault:

- **`github-terraform`**, fields:
  - `token`: a fine-grained personal access token, resource owner **opennova-net**, with
    organization permissions **Administration**, **Members** and **Self-hosted runners**
    (each Read and write) and, on **All repositories**, **Administration**,
    **Environments** (the deploy environments' variables) and **Secrets** (Read and write).
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
./deploy/run.sh github output           # the runners' names
```

`infra/github/imports.tf` adopted what existed before the stack. A new repository, member
or secret is a change to the stack: add it, plan, apply. A new image replaces a runner.

## Operating

Every command here takes the deployment it reads or changes: `--deployment staging` or
`--deployment prod`.

- **Logs.** `./deploy/run.sh --deployment <deployment> app logs [PROGRAM|backup] [--follow] [--since 2h]` reads
  CloudWatch (`/opennova/<deployment>/service`, 30 days, a stream per container and task,
  `<program>/<container>/<task id>`: web's task writes `web/web/...` and
  `web/portal/...`; a program's name tails all of its tasks').
- **Alarms** go to the deployment's SNS topic, `opennova-<deployment>-alerts` in its account, which emails
  `alerts/email` (confirm the subscription from the first email AWS sends): per program,
  no heartbeat for 5 minutes, more boots in 15 minutes than its `max` and 3 (a crash
  loop), fewer tasks running than its `min` for 5 minutes (these two once the programs
  have a release: before the first `app deploy` no task runs, and silence is no alarm),
  and its CPU or its memory over 80% for 15 minutes; a load balancer target group (the gate, the lobby, 443, the retail port) with
  an unhealthy target for 10 minutes; a failed hosted server, a lost machine, 3 failed
  launches in 15 minutes, a billing sync older than an hour, 5 refused webhooks in 15
  minutes, the entitlement stops held for 5 minutes (`limits_held`: the supervisor's
  mass-stop guard keeps over-entitled servers running until billing is trusted again;
  the admin billing view says why), a gate probe left unanswered in 5 minutes
  (`probes_unanswered`: the gate had no lobby to hand out, so no client can log in while
  every health check passes; `LobbiesLive` and `app logs lobby` say why); the database's CPU over 80% for 15 minutes, its CPU
  credit balance low (a burstable class), its connections over 80% of `max_connections`
  and its free storage; a cache node evicting keys or short of CPU credits; and the Route
  53 health check on `http://<the gate's address>:8080/healthz`, the way a stock client
  comes. A task failing its health check is its service's to replace. The heartbeat is
  each process's JSON line every 60 s, `{"heartbeat":{"program":"...",...}}`; its fields
  are metrics in `OpenNova/<deployment>`, by `Program`, and each boot line,
  `{"boot":{"program":"..."}}`, counts a boot.
- **Budgets**, each deployment's in its own account, counting that account (every cost
  in it is the deployment's): the month's cost (`budget_monthly_usd`: prod 350, staging
  250) and the EC2 data transfer out (`budget_egress_gb`): an email at 80% and at a 100%
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

About $190 a month for prod's service tier before the machines (approximate: us-east-1
on-demand list prices, rounded: Fargate Linux x86 $0.04048 a vCPU-hour and $0.004445 a
GB-hour, a public IPv4 address $0.005 an hour, a network load balancer $0.0225 an hour
and its LCUs, `db.t4g.small` Multi-AZ $0.064 an hour, RDS gp3 Multi-AZ $0.23 a GB-month,
`cache.t4g.micro` Valkey $0.0128 an hour, 730 hours a month):

| | a month |
|---|---|
| the gate, a 0.25 vCPU / 0.5 GiB task and its public IPv4 | $13 |
| the lobby, the same | $13 |
| web, a 0.5 vCPU / 1 GiB task (the server and nginx) and its public IPv4 | $22 |
| the controller, a 0.25 vCPU / 0.5 GiB task and its public IPv4 | $13 |
| the worker, a 0.5 vCPU / 1 GiB task and its public IPv4 | $22 |
| the load balancer, its LCUs at this traffic and its two Elastic IPs | $25 |
| the database, `db.t4g.small` Multi-AZ (two instances), 20 GiB gp3 each, backups | $51 |
| the cache, two `cache.t4g.micro` Valkey nodes | $19 |
| CloudWatch logs, metrics, about 45 alarms; the health check; Cloud Map's zone | $12 |
| S3 (releases, dumps, baked images), SSM, the nightly backup task, the schedule | about $1 |

Each further web or worker task adds about $22, a further lobby about $13 (staging, with
its second web task, about $210 while it runs). A deployment's extra tasks (every
program's new tasks start beside the old) run for minutes. The database adds its surplus CPU credits, if any (unlimited mode: about $0.075 a
vCPU-hour past its baseline over a day), none while it stays under its baseline. The
monthly budget of prod's account (`budget_monthly_usd`, 350) leaves about $160 for the
machines and their egress; staging's account has its own (250).

The machines: a full shared machine (`t3a.large`, its address and volumes, about $60) is
about $5 a server at 12 servers, before egress; an own machine (`c7a.medium`) about $43.
Egress is about $0.09 a GB, and a full 64-player server sends about 1.8 GB an hour, so
egress can cost more than the machine: the egress budget is the alarm. The first shared
machine runs at a loss until it fills.

## Moving prod into its account

Once: prod moves out of the management account, where the single service instance stack
runs it, into `opennova-prod`, and staging is built fresh in `opennova-staging` first, as
the rehearsal. The database's rows, the gate's address and the hosted servers' records
move; every other resource is made anew in the new accounts. The maintainer runs every
step; the destructive ones are marked. Who runs what:

- **[admin]**: the management account's administrator (the root user or an admin role),
  with the AWS CLI outside the toolbox;
- **[old]**: the toolbox of a checkout of the commit prod runs now (the last `master`
  before the programs: `git worktree add ../opennova-old <that commit>`), run here as
  `../opennova-old/deploy/run.sh`, which acts in the management account as the old
  deploy user, `onnet`, and keeps its state in the vault documents `tfstate` and
  `tfstate-staging`;
- **[new]**: this checkout's toolbox.

The three counts the database's move is checked by, before and after:

```bash
COUNTS="SELECT (SELECT count(*) FROM players) AS players, (SELECT count(*) FROM cd_keys) AS cd_keys, (SELECT count(*) FROM billing_subscriptions) AS subscriptions, (SELECT count(*) FROM hosted_servers) AS hosted_servers"
```

### 1. Before: the accounts and the shared stacks

1. Sections 1 to 3, steps 1 to 4: the vault items, the accounts, `opennova-org`, `org
   apply`.
2. **The zone's settings move into `infra/zone`.** The old prod stack holds them; a zone
   has one ruleset per phase, so the new stack adopts them rather than make them again.
   - [new] `./deploy/run.sh --deployment staging secrets init`: `app-staging` gets an
     edge secret of its own (prod's, `app-prod/edge_auth_secret`, stays what it is).
   - Their ids, from the old state (on your machine, signed in to 1Password):

     ```bash
     op document get tfstate --vault OpenNova-Deploy | jq -r '.resources[]
       | select(.type | test("^cloudflare_(ruleset|email_routing)"))
       | .type + "." + .name + "  " + (.instances[] | (.index_key // "" | tostring) + "  id=" + .attributes.id + "  account=" + (.attributes.account_id // ""))'
     ```
   - [old] `../opennova-old/deploy/run.sh infra state-rm 'cloudflare_zone_settings_override.zone[0]'
     'cloudflare_ruleset.edge_auth[0]' 'cloudflare_ruleset.webhook_skip[0]'
     'cloudflare_email_routing_settings.zone[0]' 'cloudflare_email_routing_address.forward_to[0]'
     'cloudflare_email_routing_rule.route["support"]' 'cloudflare_email_routing_rule.route["aws-prod"]'
     'cloudflare_email_routing_rule.route["aws-staging"]'` (every routing rule the old
     state holds: the objects stay; the old stack forgets them, and never plans them
     again).
   - [new] each into the zone's stack. Two are made anew instead: the zone's settings
     override holds no object to adopt, and the provider cannot import Email Routing's
     settings (the apply turns on what is on already). Strip any carriage return from an
     id read on Windows:

     ```bash
     ./deploy/run.sh zone import cloudflare_ruleset.edge_auth zone/<zone id>/<edge_auth id>
     ./deploy/run.sh zone import cloudflare_ruleset.webhook_skip zone/<zone id>/<webhook_skip id>
     ./deploy/run.sh zone import 'cloudflare_email_routing_address.forward_to[0]' <account>/<forward_to id>
     for r in support aws-prod aws-staging; do
       ./deploy/run.sh zone import "cloudflare_email_routing_rule.route[\"$r\"]" <zone id>/<that rule's id>
     done
     ./deploy/run.sh zone plan
     ./deploy/run.sh zone apply                  # type zone
     ```

     The plan rewrites the edge rule into one rule per deployment (prod's hostnames keep
     prod's secret: the old prod site keeps working; staging's hostname gets staging's)
     and adds the root addresses' routing rules. The old staging site's `/api/` stops
     here (its nginx holds prod's secret); it is retired next.
3. **The GitHub organization's state moves** from its vault document into prod's state
   bucket, as prod's deploy user (`infra/github/terraform.tfstate`; the state passes on a
   pipe, never your disk), then its new deploy environments are applied:

   ```bash
   op document get tfstate-github-org --vault OpenNova-Deploy \
     | ./deploy/run.sh --deployment prod aws s3 cp - "s3://$(op read op://OpenNova-Deploy/aws-prod/state_bucket)/infra/github/terraform.tfstate"
   ./deploy/run.sh github plan                   # the environments staging and prod, their variables
   ./deploy/run.sh github apply                  # type github
   ```

   If the old workflow's environment, `production`, exists, delete it in the repository's
   settings (Environments).

### 2. Staging, in its account: the rehearsal

1. **Destructive. [admin, old] The old staging stack goes** (the management account's
   `staging` workspace; its data is a rehearsal's, kept nowhere):

   ```bash
   # [admin]
   aws rds modify-db-instance --db-instance-identifier opennova-server-staging-db --no-deletion-protection --apply-immediately
   aws rds delete-db-instance --db-instance-identifier opennova-server-staging-db --skip-final-snapshot --delete-automated-backups
   aws rds wait db-instance-deleted --db-instance-identifier opennova-server-staging-db
   # [old]
   ../opennova-old/deploy/run.sh --deployment staging infra state-rm aws_db_instance.main
   ../opennova-old/deploy/run.sh --deployment staging infra destroy
   ```

   `onnet` may not detach the instance's network interface or disassociate its address
   (the destroy stops on `UnauthorizedOperation`); the instance it terminated freed both, so
   run the same `infra destroy` again for what is left.
2. **[new] Staging, built:**

   ```bash
   D='--deployment staging'
   ./deploy/run.sh $D secrets sync
   ./deploy/run.sh $D infra apply                 # the whole stack, every program at no task
   ./deploy/run.sh $D db bootstrap
   ./deploy/run.sh $D stripe check                # test mode, staging's site; stripe setup for what lacks
   HOSTED_DATA_DIR="<the JO:CA folder>" ./deploy/run.sh $D hosted data-snapshot --data-set jop_2_consumer
   HOSTED_DATA_DIR="<the DFX2 folder>" ./deploy/run.sh $D hosted data-snapshot --data-set dfx2_consumer
   #   each id into hosted-staging/data_snapshot_id_<slug> (opennova_0_consumer the same way)
   ./deploy/run.sh $D infra apply                 # the fleet's templates
   ```

   An EBS snapshot under the account's AWS-managed key (`aws/ebs`) cannot be shared with
   another account (moving it would mean re-encrypting it under a customer-managed key
   first), so each account makes its own from the game folders.
3. **The database's move and the first release, rehearsed** in the window's order (its
   steps 9 to 11): prod's live dump restored into the empty database before any program
   has booted on it, then the first release, whose boot applies the migrations the dump
   predates. (Prod's data reaching staging is the restore drill's, by `backup import`
   alone.)

   ```bash
   ../opennova-old/deploy/run.sh backup now                      # [old] a dump of prod, the old stack serving on
   ../opennova-old/deploy/run.sh backup list                     # its key: novaworld/novaworld-<time>.dump
   aws s3 cp s3://opennova-backups/novaworld/novaworld-<time>.dump - \
     | ./deploy/run.sh $D backup import - --as novaworld-<time>.dump    # [admin]'s aws: the dump, on a pipe
   ./deploy/run.sh $D backup restore novaworld/novaworld-<time>.dump --yes-overwrite-database
   ../opennova-old/deploy/run.sh db psql -c "$COUNTS"
   ./deploy/run.sh $D db psql -c "$COUNTS"                      # the same, but for writes since the dump
   ./deploy/run.sh $D app deploy --tag sha-<the programs> --serve-tag sha-<serve> --hostd-tag sha-<serve>
   ./deploy/run.sh $D fleet bake-image
   ```

   The restore comes first because a program that booted on the empty database would
   have applied every migration there, and a dump older than them restored over it
   (`pg_restore --clean` drops and remakes only the dump's own objects) would leave the
   newer migrations' tables beside a migration record that names none of them.
4. **The address's transfer, rehearsed** on a scratch address:

   ```bash
   aws ec2 allocate-address --domain vpc                         # [admin]: its AllocationId and PublicIp
   aws ec2 enable-address-transfer --allocation-id <scratch id> --transfer-account-id <aws-staging/account_id>   # [admin]
   ./deploy/run.sh $D aws ec2 accept-address-transfer --address <scratch ip> \
     --tag-specifications 'ResourceType=elastic-ip,Tags=[{Key=opennova:deployment,Value=opennova-staging}]'
   ./deploy/run.sh $D aws ec2 release-address --allocation-id <its new id>    # destructive: the scratch address
   ```
5. **The smoke tests** (below), A to K, on staging.

### 3. Prod, ahead of the window (no outage)

```bash
P='--deployment prod'
./deploy/run.sh $P secrets sync
HOSTED_DATA_DIR="<the JO:CA folder>" ./deploy/run.sh $P hosted data-snapshot --data-set jop_2_consumer
#   ... each data set, each id into hosted-prod/data_snapshot_id_<slug>
./deploy/run.sh $P infra apply -target=aws_db_instance.main -target=aws_elasticache_replication_group.cache -target=aws_s3_bucket.ops
```

The targeted apply makes the network, the database, the cache and the operations bucket,
the parts that take long and need no gate address (the full plan stops at its
precondition until the address is prod's).

[admin] **The address can transfer.** EC2 refuses to transfer an Elastic IP that has a
reverse DNS record, so check it has none (an empty `DomainName`), and clear one if it
does:

```bash
aws ec2 describe-addresses-attribute --allocation-ids <old allocation id> --attribute domain-name
aws ec2 reset-address-attribute --allocation-id <old allocation id> --attribute domain-name   # only if it names one
```

A transfer offer (window step 5) lapses after 7 days unaccepted; steps 5 and 6 still run
back to back, and a lapsed offer is made again with the same `enable-address-transfer`.

[admin] **`onnet` may make the moves.** The calls marked [admin] that touch the address, and
the old stack's destroys, run as `onnet` (the vault item `aws`) with one inline policy it
lacks, granted once (it goes when `onnet` does, "Cleaning the management account"):

```bash
aws iam put-user-policy --user-name onnet --policy-name opennova-cutover --policy-document '{
  "Version": "2012-10-17",
  "Statement": [
    {"Sid": "GateAddressTransfer", "Effect": "Allow", "Resource": "*",
     "Action": ["ec2:EnableAddressTransfer", "ec2:DisableAddressTransfer", "ec2:DescribeAddressTransfers",
                "ec2:DescribeAddressesAttribute", "ec2:ResetAddressAttribute"],
     "Condition": {"StringEquals": {"aws:RequestedRegion": "us-east-1"}}},
    {"Sid": "OldStackTeardown", "Effect": "Allow", "Resource": "*",
     "Action": ["ec2:DetachNetworkInterface", "ec2:DisassociateAddress"],
     "Condition": {"StringEquals": {"aws:RequestedRegion": "us-east-1"}}}
  ]}'
```

### 4. The window

The gate and the site are down from step 2 until step 10, about an hour.

1. Maintenance on (the old admin "NovaWorld service" page) and **stop all** hosted servers
   (the kill switch): their machines end; the servers' records (owners, data sets,
   settings) ride the dump. Machines cannot move between accounts, and their hostd could
   not reach the new controller anyway: the servers move the way a roll moves them,
   started anew on new machines (step 12). Wait until `../opennova-old/deploy/run.sh aws
   ec2 describe-instances --filters Name=tag:opennova:hosted,Values=1
   Name=instance-state-name,Values=running` lists none.
2. [old] `../opennova-old/deploy/run.sh app down --yes`: the old programs stop; nothing writes the
   database any more.
3. [old] The final dump and its counts: `../opennova-old/deploy/run.sh backup now`, `backup list` (the
   newest key), `db psql -c "$COUNTS"` (keep the output).
4. **Destructive.** [old] The old site's and mail's records go (the new stack makes its
   own, and an A record and a CNAME cannot share a name): `../opennova-old/deploy/run.sh infra apply
   -destroy -target='cloudflare_record.web_root[0]' -target='cloudflare_record.web_www[0]'
   -target='cloudflare_record.mail_from_mx[0]' -target='cloudflare_record.mail_from_spf[0]'`
   (type yes).
5. **The gate's address leaves the management account.** [admin], with the old
   allocation id (the old checkout's `infra/aws/prod.tfvars`, `service_eip_allocation_id`):

   ```bash
   aws ec2 describe-addresses --allocation-ids <old allocation id> --query 'Addresses[0].[PublicIp,AssociationId]' --output text
   aws ec2 disassociate-address --association-id <that association>
   aws ec2 enable-address-transfer --allocation-id <old allocation id> --transfer-account-id <aws-prod/account_id>
   ```

   Accept it now (step 6); the offer would lapse after 7 days.
6. [new] The prod account accepts it, and the vault records its allocation id there:

   ```bash
   ./deploy/run.sh $P aws ec2 accept-address-transfer --address <the gate's address> \
     --tag-specifications 'ResourceType=elastic-ip,Tags=[{Key=opennova:deployment,Value=opennova-prod}]'
   #   its AllocationId into aws-prod/gate_eip_allocation_id
   ```
7. [new] `./deploy/run.sh $P infra apply` (type prod): the whole stack, every program at
   no task (nothing runs before the first release); the load balancer takes the gate's
   address in zone a, where it carries the gate (UDP 7597), the lobby (UDP 64206) and web
   (TCP 8080, and 443 from Cloudflare); the site's records (CNAMEs to the balancer) and the
   mail's.
8. [new] `./deploy/run.sh $P db bootstrap`.
9. [new] The database's rows, from the final dump (as rehearsed):

   ```bash
   aws s3 cp s3://opennova-backups/novaworld/novaworld-<time>.dump - \
     | ./deploy/run.sh $P backup import - --as novaworld-<time>.dump --yes    # [admin]'s aws
   ./deploy/run.sh $P backup restore novaworld/novaworld-<time>.dump --yes-overwrite-database   # type prod
   ./deploy/run.sh $P db psql -c "$COUNTS"         # exactly step 3's
   ```
10. [new] `./deploy/run.sh $P app deploy --promote staging` (type prod): staging's
    rehearsed release, by digest; every program starts at its count, the migrations the
    dump predates run at boot.
11. [new] `./deploy/run.sh $P fleet bake-image`; the smoke tests A to E on prod (with
    `--deployment prod`); maintenance off in the new admin page (the dump carried it on).
12. The hosted servers: their owners (or an admin, server by server) start them again
    from My servers; each lands on a machine of the prod account (smoke tests F, G).

**Rollback** (until step 11; the old database is untouched since step 2): [new] `$P app
down --yes`; lift the load balancer's deletion protection (`$P aws elbv2
modify-load-balancer-attributes --load-balancer-arn "$(./deploy/run.sh $P infra output
-raw lb_arn)" --attributes Key=deletion_protection.enabled,Value=false`) and destroy it
and the new records: `$P infra apply -destroy --allow-destroy -target=aws_lb.public
-target='cloudflare_record.web_root[0]' -target='cloudflare_record.web_www[0]'
-target='cloudflare_record.mail_from_mx[0]' -target='cloudflare_record.mail_from_spf[0]'`;
the address back: [admin] in the prod account (the portal, `OpenNovaAdmin`) `aws ec2
enable-address-transfer --allocation-id <its prod id> --transfer-account-id <the
management account>`, then in the management account `aws ec2 accept-address-transfer
--address <the gate's address>`, whose new allocation id goes in the old checkout's
`infra/aws/prod.tfvars` (a local edit); then [old] a targeted apply that puts the address
back on the instance and remakes the site's and the mail's records, and touches nothing
else:

```bash
../opennova-old/deploy/run.sh infra apply -target=aws_eip_association.service \
  -target='aws_ec2_tag.service_eip[0]' -target='cloudflare_record.web_root[0]' \
  -target='cloudflare_record.web_www[0]' -target='cloudflare_record.mail_from_mx[0]' \
  -target='cloudflare_record.mail_from_spf[0]'                                 # type yes
../opennova-old/deploy/run.sh app converge
```

Targeted, because the old configuration still declares the zone's settings, rulesets
and Email Routing that section 1 moved to `infra/zone` and forgot in the old state: an
untargeted apply would try to make them again, against the zone stack's own (a zone holds
one ruleset per phase). Then maintenance off.

## Cleaning the management account

Once prod has run from its account long enough to trust it (its nightly dumps and its
point-in-time recovery hold the data), nothing of OpenNova stays in the management account
but the Organization, Identity Center and `opennova-org`. Every step is destructive and
the maintainer's.

1. **The old dumps worth keeping** go to prod's dumps first (a dump each, on a pipe):

   ```bash
   # [admin]'s aws reads the old bucket; the new toolbox writes prod's
   for key in $(aws s3api list-objects-v2 --bucket opennova-backups \
                  --prefix novaworld/ --query 'Contents[].Key' --output text); do
     aws s3 cp "s3://opennova-backups/$key" - \
       | ./deploy/run.sh --deployment prod backup import - --as "${key##*/}" --yes
   done
   ```

   The downloads: the stack's downloads bucket and its CloudFront distribution were
   retired before this move. Check none remains (`aws s3api list-buckets`, `aws cloudfront
   list-distributions`); were one to, a bucket's name is global: copy its objects out,
   delete it here, make it in the prod account (the name frees within about an hour), and
   move a CloudFront alias with `aws cloudfront associate-alias` run against the new
   distribution.
2. **[admin] The old database**, its backups and its snapshots, first: the stack's
   destroy cannot take its subnet group, parameter group, security group or VPC while the
   instance stands.

   ```bash
   aws rds modify-db-instance --db-instance-identifier opennova-server-db --no-deletion-protection --apply-immediately
   aws rds delete-db-instance --db-instance-identifier opennova-server-db --skip-final-snapshot --delete-automated-backups
   aws rds wait db-instance-deleted --db-instance-identifier opennova-server-db
   aws rds describe-db-snapshots --snapshot-type manual --query 'DBSnapshots[].DBSnapshotIdentifier'   # each: delete-db-snapshot
   ```
3. **The old stack**, by its own Terraform. In the old checkout (local edits, never
   committed): delete the `service_eip_allocation_id` line from `infra/aws/prod.tfvars`
   (the address is prod's now). Then [old], forgetting the address's association and tag
   (prod's now), the database (gone) and the operations bucket with every part of it, so
   the destroy never suspends `opennova-backups`' versioning or lifts its public access
   block before step 4 empties it:

   ```bash
   ../opennova-old/deploy/run.sh infra state-rm aws_eip_association.service 'aws_ec2_tag.service_eip[0]' \
     aws_db_instance.main 'aws_s3_bucket.ops[0]' 'aws_s3_bucket_versioning.ops[0]' \
     'aws_s3_bucket_lifecycle_configuration.ops[0]' 'aws_s3_bucket_public_access_block.ops[0]' \
     'aws_s3_bucket_server_side_encryption_configuration.ops[0]'
   ../opennova-old/deploy/run.sh infra apply -destroy --allow-destroy        # type yes
   ```

   If it stops on `UnauthorizedOperation` detaching the instance's network interface,
   `onnet` lacks that call; the terminated instance frees the interface, so run the same
   command again. What a plan still cannot destroy goes by hand in the steps below.
4. **[admin] The old bucket**, `opennova-backups`: the S3 console's Empty (it removes every
   version), then Delete.
5. **[admin] IAM.** The old deploy user `onnet`: each access key (`aws iam
   list-access-keys --user-name onnet`, `delete-access-key`), each attached policy
   (`list-attached-user-policies`, `detach-user-policy`), each inline one
   (`list-user-policies`, `delete-user-policy`), then `aws iam delete-user --user-name
   onnet`. The customer-managed policies `onnet-deploy-*` and the old
   `opennova-role-boundary` (`aws iam list-policies --scope Local`, `delete-policy`, its
   non-default versions first). Any other OpenNova user, such as an old CI user (`aws iam
   list-users`: every user but `opennova-org`; check `gh secret list` names none of their
   keys first), and any role left (`aws iam list-roles --query
   "Roles[?starts_with(RoleName, 'opennova')].RoleName"`).
6. **The vault.** Delete the documents `tfstate`, `tfstate-staging` and `tfstate-github-org`
   (moved: "Moving prod into its account", step 1.3); the items `aws` (onnet's key), `rds`
   and `hosted` (the old stack's), `ghcr` (the old toolbox's image owner) and `ssh` (the
   retired instance's key pair); and `stripe-test`'s field `ci_secret_key` (the full test
   key CI used, replaced by `restricted_key`). Every other item is in use: check with
   `git grep -E "op://|item_json|vault_set_fields"` before deleting anything else.
7. **[admin] Check that nothing of ours is left**, in each region the stack used
   (us-east-1): every resource the stack made carried `opennova:deployment`, so

   ```bash
   aws resourcegroupstaggingapi get-resources --tag-filters Key=opennova:deployment \
     --query 'ResourceTagMappingList[].ResourceARN'                      # []
   ```

   and, for what carries no tag or outlives one, each of these lists nothing of
   OpenNova's:

   ```bash
   aws ec2 describe-instances --query 'Reservations[].Instances[?State.Name!=`terminated`].InstanceId'
   aws ec2 describe-addresses
   aws ec2 describe-vpcs --filters Name=is-default,Values=false
   aws ec2 describe-snapshots --owner-ids self
   aws ec2 describe-images --owners self
   aws ec2 describe-launch-templates
   aws rds describe-db-instances; aws rds describe-db-snapshots --snapshot-type manual
   aws elasticache describe-replication-groups
   aws s3api list-buckets
   aws ssm get-parameters-by-path --path /opennova --recursive --query 'Parameters[].Name'
   aws logs describe-log-groups --log-group-name-prefix /opennova
   aws cloudwatch describe-alarms --alarm-name-prefix opennova --query 'MetricAlarms[].AlarmName'
   aws sns list-topics
   aws route53 list-health-checks; aws route53 list-hosted-zones
   aws sesv2 list-email-identities
   aws acm list-certificates                       # the old downloads.opennova.net certificate too
   aws budgets describe-budgets --account-id <the management account>
   aws ce get-anomaly-monitors
   aws ecs list-clusters; aws scheduler list-schedules
   aws iam list-users; aws iam list-roles --query "Roles[?starts_with(RoleName, 'opennova')]"
   aws iam list-policies --scope Local
   ```

## Smoke tests

On the deployment the tests name (`--deployment staging` or `--deployment prod` on
every command).

- **A. The stack's front.** `curl -I https://<domain>/` answers 200; `curl -i
  http://<the gate's address>:8080/healthz` answers 200 `ok` (and `/livez` too); `curl -i
  http://<the gate's address>:8080/jop_2.gsb` answers 200; `curl -i http://<the gate's
  address>:8080/api/health` answers 403 (`proxy_required`); `curl -m 5 http://<the gate's
  address>/` times out (no port 80); `curl https://<site host>/api/server-info` names the
  gate's Elastic IP; `./deploy/run.sh --deployment <deployment> aws ec2 describe-addresses
  --allocation-ids <id>` shows it on the load balancer's zone-a interface.
- **B. Operations.** `app status` shows every program's service at its count, its
  rollout COMPLETED on the release's revision; `app logs` shows a stream per container and
  task; the `Heartbeat` metric has points for each `Program`; the SNS email subscription
  is confirmed; the Route 53 health check is green; the load balancer's four target
  groups are healthy (EC2, Target groups); `app shell controller` opens a shell; `backup
  now` writes a dump.
- **C. Accounts.** Register, log in, log out; the admin pages open.
- **D. Billing.** Stripe's "Send test webhook" answers 2xx; subscribe to Tier 1 ($5, test
  card in test mode) or give a comp grant; the account's entitlement shows it.
- **E. Retail.** A stock JO client signs in through the gate and lists the lobby (at the
  gate's address, port 64206; `app logs lobby` names its session); its HTTP requests
  reach the gate's address on 8080. On staging, with two web tasks, repeat the login until its steps land
  on both (web's logs show it): it completes.
- **F. A shared machine.** Start a server: a shared machine launches (from the baked
  image, `fleet status`), its hostd reaches the controller by its Cloud Map name, the
  server lands on slot k and is joinable by a retail and an OpenNova client.
- **G. A second account** starts one: it lands on slot k+1 of the same machine.
- **H. Deploys that keep the lobby.** With a server listed and a retail client in the
  lobby: `app deploy --image portal=<tag>` (nothing restarts but web's task), then `app
  restart web`, `app restart worker`, `app restart controller` and `app restart gate`:
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
