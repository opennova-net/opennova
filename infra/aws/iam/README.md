# The OpenNova accounts' IAM

OpenNova runs in an AWS Organization: the management account holds the organization,
billing and one principal, `opennova-org`; each deployment runs in a member account of its own,
`opennova-prod` and `opennova-staging` (DEPLOY.md, "Deployments"), and the CI runners in a third,
`opennova-ci` (`infra/ci`; DEPLOY.md, "CI runners"). This directory holds every
IAM document those principals carry. No document names an account: each is a template, filled
with the account's id and region from the vault.

| Document | Who carries it | Applied by |
| --- | --- | --- |
| `opennova-org.json` | the IAM user `opennova-org` in the management account | the management account's administrator, by hand (below) |
| `opennova-deploy-network.json`, `-compute`, `-ecs`, `-iam`, `-data`, `-observability` | the IAM user `opennova-deploy` in each member account | the org stack (`infra/org`), as managed policies |
| `opennova-role-boundary.json` | every role `infra/aws` makes in a member account but the machines' two (permissions boundary), and the GitHub deploy role | the org stack |
| `opennova-machine-boundary.json` | the machines' instance role and hostd's task role (permissions boundary) | the org stack |

## opennova-org

The org stack's principal (`./deploy/run.sh org plan|apply`; its key in the vault item `aws-org`),
and nothing else: it reads the organization, makes and tags member accounts
(`organizations:CreateAccount`, `TagResource`, `UntagResource` on the organization's accounts),
and assumes each member account's `OrganizationAccountAccessRole`, the administrator role
Organizations made with the account. Every resource the org stack makes in a member account, and
the org stack's own state (in prod's state bucket), it reaches through that role; so too the CI
runners' stack (`./deploy/run.sh ci plan|apply`) and its state, beside the org stack's. A new account
adds its role to the document (the org stack's output `org_user_policy` prints the filled
document after an apply).

Make it once, as the management account's administrator (not the toolbox):

```bash
V=OpenNova-Deploy
policy="$(sed -e "s/\${management_account_id}/$(op read "op://$V/aws-org/account_id")/" \
              -e "s/\${organization_id}/$(op read "op://$V/aws-org/organization_id")/" \
              -e "s/\${prod_account_id}/$(op read "op://$V/aws-prod/account_id")/" \
              -e "s/\${staging_account_id}/$(op read "op://$V/aws-staging/account_id")/" \
              -e "s/\${ci_account_id}/$(op read "op://$V/aws-ci/account_id")/" \
              infra/aws/iam/opennova-org.json)"
aws iam create-user --user-name opennova-org
aws iam put-user-policy --user-name opennova-org --policy-name opennova-org --policy-document "$policy"
# its key straight into the vault item aws-org, never onto the screen:
aws iam create-access-key --user-name opennova-org --output json \
  | jq -r '"access_key_id=\(.AccessKey.AccessKeyId)\nsecret_access_key=\(.AccessKey.SecretAccessKey)"' \
  | while IFS='=' read -r field value; do op item edit aws-org --vault "$V" "$field[concealed]=$value" >/dev/null; done
```

Changing it later is the same `put-user-policy` with the edited document.

## The deploy user and its policies

`opennova-deploy`, one per member account, is what the toolbox and every `infra/aws` run act as
in that account: the org stack makes it, its key, and its policies, and the toolbox writes the
key into the deployment's vault item (`aws-<deployment>`) from the stack's sensitive output,
never printed. The account holds one deployment alone, yet the policies keep their scoping to
OpenNova's names (`opennova-*`) and its deployment tag (`opennova:deployment`, `opennova-prod` or
`opennova-staging`), which keeps the deploy user off what the org stack made there (the state
bucket's settings, its own definition, the boundaries) and off anything an administrator adds by
hand.

| Policy | Covers |
| --- | --- |
| `opennova-deploy-network` | VPC, subnets, route tables, internet gateway, security groups and rules, network interfaces, Elastic IPs (allocating one with the deployment's tag, associating, tagging, accepting a transferred address, releasing any but prod's tagged ones); creating launch templates; the public network load balancer, its target groups and listeners |
| `opennova-deploy-compute` | EC2 reads, instances and their volumes, changing and deleting launch templates, EC2 tags, deregistering OpenNova-tagged images and deleting their snapshots (the cutover's cleanup of the baked fleet images), `kms:DescribeKey` |
| `opennova-deploy-ecs` | the ECS clusters (the programs' and the fleet's, where hostd runs), their services, task definitions and tasks (one-off tasks, ECS Exec), Application Auto Scaling, Cloud Map, EventBridge Scheduler (the nightly backup) |
| `opennova-deploy-iam` | the stack's roles (under either boundary), instance profiles, the GitHub OIDC provider, `iam:PassRole` to EC2, ECS's tasks and EventBridge Scheduler, the service-linked roles RDS, ElastiCache, Elastic Load Balancing, ECS, Application Auto Scaling and CloudWatch's alarm actions make on first use, the policy simulator; nothing about IAM users |
| `opennova-deploy-data` | RDS, ElastiCache, the operations bucket (its versioning, lifecycle and encryption; its releases read and written, its dumps read and written, never deleted; the game data, `data/*`, is the tasks' roles' alone, never the deploy user's), SSM parameters, its own sessions (ECS Exec), CloudWatch Logs |
| `opennova-deploy-observability` | alarms and metrics, the alerts topic, the Route 53 health check, the private zone Cloud Map keeps for the fleet program and the static addresses' public zone (`play.<domain>`, `infra/aws/edge.tf`), budgets, Cost Anomaly Detection, Service Quotas and cost reads, SES identities |

Beside them the org stack gives the user two inline policies: its Terraform states (in its
account's state bucket, `infra/aws/*`; prod's also `infra/zone/*` and `infra/github/*`, the shared
stacks; never `org/*`, the org stack's own) with their lock files, and a guard denying it the
state bucket's settings and old versions. The state bucket's own policy admits the deploy user,
the account's `OrganizationAccountAccessRole` and its root alone, over TLS alone.

Each document stays under IAM's 6,144-character limit for a managed policy (white space aside).

## The boundaries

The deploy user writes the inline policies of the roles it creates and runs tasks with them, so
without a boundary it could write any policy onto a task role and read its credentials in a
task. It may create a role, or put or change its policy or trust, only when the role carries
one of the two boundaries below (`iam:PermissionsBoundary`, which `opennova-deploy-iam` names
both of), and it can neither attach managed policies, create or edit policies, nor take a
boundary off. Each is the union of what the roles that carry it need; each role's own policy
narrows its part. When a role gains a permission, its boundary must gain it too, or the role is
refused at run time (an AccessDenied naming the boundary).

| Boundary | Carried by | Holds |
| --- | --- | --- |
| `opennova-machine-boundary` | the machines' two roles: the instance role (`opennova-<deployment>-machine`) and hostd's task role (`opennova-<deployment>-hostd`) | the ECS agent's calls on the fleet clusters (`opennova-*-fleet`) and their container instances (registering the machine, reporting its tasks and containers, polling for its work, its telemetry session; finding the poll endpoint), ECS Exec's session channels (`fleet shell`), the log streams of the `/opennova/*/machines` and `/opennova/*/servers` groups (written by the docker daemon's awslogs driver: hostd's own and the slots'), and the trees of the store (`data/trees/` of an `opennova-*` bucket, listed and read) |
| `opennova-role-boundary` | every other role `infra/aws` makes (the programs' task roles, the execution role, the backup task's and its schedule's), and the GitHub deploy role | the session channels (ECS Exec), `/opennova/*` parameters and their names, the operations bucket (its objects read, written and deleted, its uploads in flight listed and aborted: the game data's store, `data/`, which web's and the data program's task roles narrow to), `/opennova/*` logs (their streams written, and their events read for the admin's log view), the AWS-managed keys through SSM and EC2, launching and ending machines tagged `opennova:hosted=1` from OpenNova-tagged subnets, groups and templates, the static addresses (Elastic IPs allocated tagged `opennova:hosted=1` and an `opennova-*` deployment, tagged only as they are allocated, associated with such machines, disassociated and released only while they carry both tags, and described) and their names (A records, by UPSERT and DELETE alone, named `*.play.*` in a hosted zone, and a change's state read: the worker's policy narrows them to its own play zone), passing `opennova-*` roles to EC2, ECS's tasks and EventBridge Scheduler, sending mail through the account's SES identities, running `opennova-*` task definitions in an `opennova-*` cluster and tagging their tasks, registering task definitions, pointing `opennova-*` services at them (the fleet cluster's hostd among them) and the backup schedule at a release's revision, ECS Exec into `opennova-*` clusters' tasks (the deploy workflow's, which its own policy narrows to its deployment's web container: `app deploy`'s check of the machines' hostd image), and the admin's infrastructure view's reads (web's: listing and describing `opennova-*` clusters' tasks and container instances, the fleet cluster's among them, describing `opennova-*` databases and cache replication groups, the target groups' health, CloudWatch's metrics) |

What goes where: a permission only the machines' roles use goes in the machine boundary, and
nothing a program, the backup or the deploy workflow uses goes there; a permission both sides
use (ECS Exec's channels) is in each. A role of the machines holds nothing of the programs'
(no launch, no `iam:PassRole`, no write to the store), and no program's role holds the ECS
agent's calls.

**6,144 characters.** IAM takes a managed policy of at most 6,144 characters, white space
aside, counted as the org stack renders it (the account's id and region filled in, minified:
`infra/org/account/deploy.tf`); `infra/org/tests` refuses a document past it. The role
boundary is the one near the limit, so it is kept compact: the actions it allows on every
resource, under no condition, share one statement (`Unscoped`), its statement ids are short,
and two statements of the same action and resource are one (`PassRoles` names the three
services). Compacting never widens a scoped resource and never drops or loosens a condition:
two statements become one only when the one grants exactly what the two did. Statements under
the same conditions share one where no action of one applies to the other's resources
(`LaunchTagged`: `ec2:RunInstances` makes instances, never Elastic IPs, and
`ec2:AllocateAddress` the reverse; `TagAtLaunch`: a tag at creation is under
`ec2:CreateAction`, the action that made what it tags; `Clusters` and `TasksDbCache`:
`ecs:ExecuteCommand` acts on a cluster and its task, never a database or a cache, and the
reads beside it never exec). A permission
that would push it past the limit and belongs to one kind of role alone is the moment for a
boundary of its own, as the machines' was.

The org stack applies both, so they go first: before the `infra apply` that turns hosting on
(`hosted_enabled = true`, DEPLOY.md "Hosted game servers"), `org apply` with this directory's
documents: the role boundary holds the store's `s3:DeleteObject` and
`s3:ListBucketMultipartUploads`, the machine boundary the ECS agent's calls; applied after,
every store call of web's and the data program's task roles is refused until it is, and no
machine role or hostd task role can be made under a boundary IAM does not hold yet (the apply
fails on them), so no machine registers in the fleet cluster. The deploy user's policies gain
nothing for the store; the fleet cluster, hostd's service and task definition and hostd's task
role fall under their `opennova-*` names and the deployment's tag as the programs' do. So too
before raising `fleet_max_addresses` above 0 (DEPLOY.md, "Turning it on"): the role boundary
holds the worker's address and record rights, or every allocation is refused (the worker's
policy names them whenever hosting is on, and the deploy user's already cover the play zone
itself).

## Changing a policy

Edit the document (and the role's boundary with it when a role needs more), then `./deploy/run.sh org
plan` and `org apply`: the org stack updates both accounts' policies. `opennova-org.json` is the
administrator's, as above.

## To verify on the first applies

The tests plan against mock providers; only a real run proves these (each would show as an
AccessDenied naming the action):

- the org stack: the import of the organization and both accounts as `opennova-org`, its
  `sts:AssumeRole` into each `OrganizationAccountAccessRole`, the backend's `assume_role`
  into prod's for `org/terraform.tfstate`;
- each account's first `infra apply` as its deploy user: the S3 backend's lock file
  (`s3:DeleteObject` on it alone), every service-linked role made on first use, the cluster
  and its services under the deployment's tag, Cloud Map's private namespace, the backup
  schedule (`iam:PassRole` to `scheduler.amazonaws.com`);
- prod's gate address: its `ec2:AllocateAddress` with the deployment's tag (or
  `ec2:AcceptAddressTransfer` of one transferred in), its tag and its association with the load
  balancer;
- the first deploy workflow run: its role's `scheduler:UpdateSchedule` under the role boundary,
  its roll of hostd (the fleet cluster's service, hostd's revision and task role), and its ECS
  Exec into a web task after it (`ecs:ListTasks`, `ecs:DescribeTasks`, `ecs:ExecuteCommand`
  under `ecs:container-name` `web`, the session plugin's `ssmmessages:OpenDataChannel`; no
  `ssm:StartSession`, which ECS makes itself): the deploy's log says "every ready machine's
  agent reports ..." rather than naming a refused call;
- the first `infra apply` with `hosted_enabled`: the game data's lifecycle rules; then, at
  the first upload on the Game data page, web's `s3:PutObject` of the parts the browser
  sends it (as its task role, under the role boundary); at the first tree deleted on the page or
  pruned by the worker, `s3:ListBucketMultipartUploads` (no prefix condition: the action's
  context carries none) and `s3:DeleteObject`; at the first server, hostd's task role's
  `aws s3 sync` of a tree (`s3:ListBucket` with `s3:prefix` `data/trees/*`, `s3:GetObject`);
- a worker's first machine launch under the role boundary (the pinned image, by `ec2:ImageID`),
  that machine's ECS agent registering in the fleet cluster and polling (the instance role's ECS
  calls under the machine boundary, and the awslogs streams of hostd and the slots), hostd's ECS
  Exec (`fleet shell`), and web's reads of the fleet cluster's container instances.
- the first `infra apply` with `fleet_max_addresses` above 0: the deploy user's making of the
  public play zone (`route53:CreateHostedZone`, its tags, `GetChange` while it syncs) and the
  Cloudflare NS records; then the worker's first static address: `ec2:AllocateAddress` with its
  tag specifications (and `ec2:CreateTags` under `ec2:CreateAction`), `ec2:AssociateAddress`
  naming the machine (whether EC2 also evaluates the machine's network interface, which carries
  no tag), its first `route53:ChangeResourceRecordSets` under the record-name condition, and a
  release's `ec2:DisassociateAddress` (whether EC2 evaluates more than the address) and
  `ec2:ReleaseAddress`;
- the admin's log and usage views on web's first read after `org apply`: web's task role's
  `logs:GetLogEvents` and `logs:FilterLogEvents` on the deployment's `service`, `machines`
  and `servers` groups and `cloudwatch:GetMetricData` (no resource scoping exists for it),
  each under the role boundary.
