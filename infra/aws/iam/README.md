# The OpenNova accounts' IAM

OpenNova runs in an AWS Organization: the management account holds the organization,
billing and one principal, `opennova-org`; each deployment runs in a member account of its own,
`opennova-prod` and `opennova-staging` (DEPLOY.md, "Deployments"). This directory holds every
IAM document those principals carry. No document names an account: each is a template, filled
with the account's id and region from the vault.

| Document | Who carries it | Applied by |
| --- | --- | --- |
| `opennova-org.json` | the IAM user `opennova-org` in the management account | the management account's administrator, by hand (below) |
| `opennova-deploy-network.json`, `-compute`, `-ecs`, `-iam`, `-data`, `-observability` | the IAM user `opennova-deploy` in each member account | the org stack (`infra/org`), as managed policies |
| `opennova-role-boundary.json` | every role `infra/aws` makes in a member account (permissions boundary), and the GitHub deploy role | the org stack |

## opennova-org

The org stack's principal (`./deploy/run.sh org plan|apply`; its key in the vault item `aws-org`),
and nothing else: it reads the organization, makes and tags member accounts
(`organizations:CreateAccount`, `TagResource`, `UntagResource` on the organization's accounts),
and assumes each member account's `OrganizationAccountAccessRole`, the administrator role
Organizations made with the account. Every resource the org stack makes in a member account, and
the org stack's own state (in prod's state bucket), it reaches through that role. A new account
adds its role to the document (the org stack's output `org_user_policy` prints the filled
document after an apply).

Make it once, as the management account's administrator (not the toolbox):

```bash
V=OpenNova-Deploy
policy="$(sed -e "s/\${management_account_id}/$(op read "op://$V/aws-org/account_id")/" \
              -e "s/\${organization_id}/$(op read "op://$V/aws-org/organization_id")/" \
              -e "s/\${prod_account_id}/$(op read "op://$V/aws-prod/account_id")/" \
              -e "s/\${staging_account_id}/$(op read "op://$V/aws-staging/account_id")/" \
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
bucket's settings, its own definition, the boundary) and off anything an administrator adds by
hand.

| Policy | Covers |
| --- | --- |
| `opennova-deploy-network` | VPC, subnets, route tables, internet gateway, security groups and rules, network interfaces, Elastic IPs (allocating one with the deployment's tag, associating, tagging, accepting a transferred address, releasing any but prod's tagged ones); creating launch templates; the public network load balancer, its target groups and listeners |
| `opennova-deploy-compute` | EC2 reads, instances and their volumes, changing and deleting launch templates, EC2 tags, the baked fleet images, `kms:DescribeKey` |
| `opennova-deploy-ecs` | the ECS cluster, its services, task definitions and tasks (one-off tasks, ECS Exec), Application Auto Scaling, Cloud Map, EventBridge Scheduler (the nightly backup) |
| `opennova-deploy-iam` | the stack's roles (under the boundary), instance profiles, the GitHub OIDC provider, `iam:PassRole` to EC2, ECS's tasks and EventBridge Scheduler, the service-linked roles RDS, ElastiCache, Elastic Load Balancing, ECS, Application Auto Scaling and CloudWatch's alarm actions make on first use, the policy simulator; nothing about IAM users |
| `opennova-deploy-data` | RDS, ElastiCache, the operations bucket (its versioning, lifecycle and encryption; its releases read and written, its dumps read and written, never deleted; the game data, `data/*`, is the tasks' roles' alone, never the deploy user's), SSM parameters, Session Manager (the machines' break-glass), CloudWatch Logs |
| `opennova-deploy-observability` | alarms and metrics, the alerts topic, the Route 53 health check and the private zone Cloud Map keeps for the controller, budgets, Cost Anomaly Detection, Service Quotas and cost reads, SES identities |

Beside them the org stack gives the user two inline policies: its Terraform states (in its
account's state bucket, `infra/aws/*`; prod's also `infra/zone/*` and `infra/github/*`, the shared
stacks; never `org/*`, the org stack's own) with their lock files, and a guard denying it the
state bucket's settings and old versions. The state bucket's own policy admits the deploy user,
the account's `OrganizationAccountAccessRole` and its root alone, over TLS alone.

Each document stays under IAM's 6,144-character limit for a managed policy (white space aside).

## The role boundary

The deploy user writes the inline policies of the roles it creates and runs tasks with them, so
without a boundary it could write any policy onto a task role and read its credentials in a
task. It may create a role, or put or change its policy or trust, only when the role carries
`opennova-role-boundary` (`iam:PermissionsBoundary`), and it can neither attach managed policies,
create or edit policies, nor take a boundary off. The boundary is the union of what `infra/aws`'s
roles and the GitHub deploy role need: the session channels (ECS Exec, the machines' Session
Manager), `/opennova/*` parameters and their names, the operations bucket (its objects read,
written and deleted, its uploads in flight listed and aborted: the game data's store, `data/`,
which web's and the worker's task roles narrow to and the machine role reads the trees of),
`/opennova/*` logs, the AWS-managed keys through SSM and EC2, launching and ending machines tagged
`opennova:hosted=1` from OpenNova-tagged subnets, groups, templates and snapshots, passing
`opennova-*` roles to EC2, ECS's tasks and EventBridge Scheduler, sending mail through the
account's SES identities, running `opennova-*`
task definitions in an `opennova-*` cluster and tagging their tasks, registering task
definitions, pointing `opennova-*` services at them and pointing the backup schedule at a
release's revision, and the admin's infrastructure view's reads (web's: listing and
describing `opennova-*` clusters' tasks, describing `opennova-*` databases and cache
replication groups, the target groups' health). Each role's own policy narrows its part. When a role gains a permission,
the boundary must gain it too, or the role is refused at run time (an AccessDenied naming the
boundary). The org stack applies the boundary, so it goes first: before the `infra apply` that
turns hosting on (`hosted_enabled = true`, DEPLOY.md "Hosted game servers"), `org apply` with
this directory's `opennova-role-boundary.json`, which holds the store's `s3:DeleteObject` and
`s3:ListBucketMultipartUploads`; applied after, every store call of web's and the worker's task
roles is refused until it is. The deploy user's policies gain nothing for the store.

## Changing a policy

Edit the document (and the boundary with it when a role needs more), then `./deploy/run.sh org
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
- the first deploy workflow run: its role's `scheduler:UpdateSchedule` under the boundary;
- the first `infra apply` with `hosted_enabled`: the game data's lifecycle rules; then, at
  the first upload on the Game data page, web's `s3:PutObject` of the parts the browser
  sends it (as its task role, under the boundary); at the first tree deleted on the page or
  pruned by the worker, `s3:ListBucketMultipartUploads` (no prefix condition: the action's
  context carries none) and `s3:DeleteObject`; at the first server, the machine role's
  `aws s3 sync` of a tree (`s3:ListBucket` with `s3:prefix` `data/trees/*`, `s3:GetObject`);
- the first `fleet bake-image`, and a worker's first machine launch under the boundary.
