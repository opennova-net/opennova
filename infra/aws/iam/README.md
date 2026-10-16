# The deploy user's permissions (`onnet`)

`onnet` is the IAM user whose access key sits in the vault's `aws` item: the deploy toolbox
(`deploy/bin/on-deploy`) and every `infra` run (Terraform in `infra/aws/`) act as it. The
account (310045376545, us-east-1) also holds other projects, so `onnet` is allowed OpenNova's
resources alone: five customer-managed policies that only allow, scoped to OpenNova's tags,
names and VPC, and one permissions boundary every role Terraform makes must carry. There are
no explicit denies. This directory is the source of truth; IAM holds the applied copies.

| Policy (file) | Attached to | Covers |
| --- | --- | --- |
| `onnet-deploy-network` | onnet | VPC, subnets, route tables, internet gateway, security groups and rules, the service's network interface, Elastic IPs; creating launch templates (with the other creates that name no parent resource) |
| `onnet-deploy-compute` | onnet | EC2 reads, the service instance and its root volume, changing and deleting launch templates, EC2 tags, the hosted data snapshots (EBS direct APIs), `kms:DescribeKey`, the single-host stack's teardown |
| `onnet-deploy-iam` | onnet | the stack's roles (under the boundary), instance profiles, the GitHub OIDC provider, `iam:PassRole` to EC2, CloudWatch's alarm-actions service-linked role, the policy simulator |
| `onnet-deploy-data` | onnet | RDS (the database, its subnet and parameter groups, snapshots), the operations bucket, SSM parameters, the converge document, Run Command and Session Manager, CloudWatch Logs |
| `onnet-deploy-observability` | onnet | alarms and metrics, the alerts topic, the Route 53 health check, budgets, Cost Anomaly Detection, Service Quotas and cost reads; the outbound mail's SES identities |
| `opennova-role-boundary` | every `aws_iam_role` in `infra/aws` (permissions boundary) | the most any OpenNova role can ever do |

Each file is under the 6,144-character managed-policy limit (IAM does not count white space),
which is what keeps EC2 in two policies.

## How "OpenNova's" is decided

- **The deployment tag.** Every resource the stack makes carries `opennova:deployment`
  (`versions.tf`'s default tag, `local.hosted_deployment`: `opennova-server` for prod,
  `opennova-server-staging` for staging), and so do the machines the service launches and the
  data snapshots. EC2 creates require it in the request (`aws:RequestTag/opennova:deployment`
  like `opennova-*`) on the resource being made, never on the parent it is made in (a subnet's
  VPC, an interface's subnet, a rule's group must themselves be OpenNova's); EC2 changes and
  deletes require it on the resource (`aws:ResourceTag/...`); and `ec2:CreateTags` is allowed
  only as part of a create (`ec2:CreateAction`) or on a resource that already carries it, so no
  other resource can be tagged into scope. Cost Anomaly Detection and the EBS direct APIs are
  scoped the same way. The policies assume the tag's value starts with `opennova-` (it does
  unless `hosted_deployment` is set to something else).
- **Names**, where the service has no tag conditions or names are the better key: IAM
  `role/opennova-*`, `instance-profile/opennova-*`, the GitHub OIDC provider; RDS `db:`,
  `subgrp:`, `pg:`, `snapshot:opennova-*` (plus the default option group
  `og:default:postgres-*`, as the parent `CreateDBInstance` names); S3 `opennova-*` (and
  `*.opennova.net`, the retired downloads bucket, in case a state still lists it); SSM
  `parameter/opennova/*` and `document/opennova-*`; logs `/opennova/*`; alarms, the SNS topic
  and budgets `opennova-*`; key pairs `opennova-*`. Object access in the bucket is by prefix:
  `releases/*` read and write, the dumps (`novaworld*`) read only, nothing deletes an object.
- **Prod's network from before the default tag.** The single-host stack's VPC
  (`vpc-039f64354e9a338de`, 10.44.0.0/16) and everything in it carry no `opennova:deployment`
  tag until the first apply of this stack adds it. Inside it, `ec2:Vpc` scopes subnets, route
  tables, security groups and interfaces (`ManageInProdVpc`); the VPC and its internet gateway
  (`igw-04b9394fa71df326a`), which have no `ec2:Vpc` key, are named by ARN
  (`ProdVpcAndGatewayUntilTagged`). Retail clients' Elastic IP (`eipalloc-05d28b7cdb8ae7c0a`,
  54.162.79.147), which prod adopts and never manages, may be associated, disassociated and
  tagged by its ARN (`aws_ec2_tag.service_eip` gives it the deployment tag), and never
  released: `ec2:ReleaseAddress` covers OpenNova-tagged addresses whose tag is not prod's
  (`opennova-server`), that is staging's.
- **The single-host stack's teardown** (`LegacyStackTeardown`, `LegacyStackPolicyTeardown`):
  the old instance `i-0ca78b471e6590605`, the hosted launch templates `lt-087331e4c404fa0a8` and
  `lt-003967f4cc2ec1ad2`, the key pair `opennova-server-key`, and the old managed policies
  `policy/opennova-server-*`. The old roles, profiles and security groups fall under the
  name and VPC rules above. These statements are dead once the cutover apply has run; drop
  them at the next policy change.
- **Reads** are on `*` where the action takes no resource (`ec2:Describe*`, `rds:Describe*`,
  `logs:DescribeLogGroups`, `cloudwatch:GetMetricData`, `ce:Get*`, `servicequotas:Get*`,
  `ssm:GetCommandInvocation`, ...). They read other projects' metadata too (an instance's user
  data among it), never their data.
- **SES identities** (`MailIdentities`): an identity's ARN is the domain or the address
  it verifies, and the forward_to address comes from the vault, so no committed policy
  can name it: the identity actions are on `identity/*` in the account. The service's
  `ses:SendEmail` is scoped in Terraform (`mail.tf`: the stack's identities, from its
  sender alone); the boundary allows it on `identity/*`.
- **Two writes IAM cannot scope:**
  - Route 53 health checks: their ARNs carry an id alone and Route 53 has no tag conditions, so
    `UpdateHealthCheck`, `DeleteHealthCheck` and `ChangeTagsForResource` are on
    `healthcheck/*` and reach other projects' checks as well. Once prod's and staging's checks
    exist (`infra output health_check_id`), an admin can narrow `HealthChecks` to their ARNs.
  - SNS subscriptions: `GetSubscriptionAttributes`, `SetSubscriptionAttributes` and
    `Unsubscribe` are authorized against `*`, not the topic (the alerts email subscription
    Terraform reads and deletes), so they reach other projects' subscriptions as well.
- **Left out on purpose:** `s3:PutAccountPublicAccessBlock` and
  `ce:UpdateCostAllocationTagsStatus` (account-wide, no resource scope: Terraform no longer
  manages the account block, and the cost tag is activated once in the switch below);
  `iam:CreatePolicy` and `iam:AttachRolePolicy` (the stack makes inline role policies only);
  ACM and CloudFront (the downloads bucket, its distribution and `launcher_ci` are gone
  already; the one `downloads.opennova.net` certificate left, `cf14fd0e-...`, is not the one the
  stack made and is for an admin to delete); and everything about IAM users and groups.

## The role boundary

`onnet` writes the inline policies of the roles it creates and can run commands on the
service instance, so without a boundary it could write any policy onto
`opennova-server-service` and read that role's credentials on the host, which would reach the
whole account. The boundary is the one guardrail: `onnet` may create a role, or put or change
its policy or trust, only when the role carries `opennova-role-boundary`
(`iam:PermissionsBoundary`), and it has no permission to attach managed policies, create or
edit policies, or remove a boundary. The boundary is the union of what the stack's three roles
need, scoped to OpenNova: SSM's agent and session channels, `/opennova/*` parameters and the
converge document, Run Command on instances tagged `opennova:service`, the operations bucket,
`/opennova/*` logs, the AWS-managed keys through SSM and EC2, launching and ending machines
tagged `opennova:hosted=1` from OpenNova-tagged subnets, groups, templates and snapshots,
passing `opennova-*` roles to EC2, and sending mail through the account's SES identities.

Every role the stack makes carries it: `permissions_boundary = local.role_boundary_arn`
(`service.tf`) on `aws_iam_role.service`, `aws_iam_role.machine` and
`aws_iam_role.github_deploy`. The boundary policy itself is made by an admin, outside
Terraform (step 1 below): a policy the deploy user could edit would bound nothing.

When a role in `infra/aws` gains a permission, the boundary must gain it too (otherwise the
role is refused at run time, an AccessDenied naming the boundary).

## Applying

Run as `onnet` itself while it still has `IAMFullAccess`, from the repository root, in this
order: a user holds at most ten managed policies, so the broad ones come off before the new
ones go on, and `IAMFullAccess` comes off last. The documents are passed inline (the shell
reads the file), so this works through the toolbox's `aws` as well.

```bash
AWS='./deploy/run.sh aws'   # or any shell holding onnet's key

# 0. the one account-wide write onnet keeps no permission for: the cost allocation tag
$AWS ce update-cost-allocation-tags-status \
  --cost-allocation-tags-status TagKey=opennova:deployment,Status=Active

# 1. the six policies
for p in onnet-deploy-network onnet-deploy-compute onnet-deploy-iam onnet-deploy-data \
         onnet-deploy-observability opennova-role-boundary; do
  $AWS iam create-policy --policy-name "$p" --policy-document "$(cat "infra/aws/iam/$p.json")" \
    --description "OpenNova deploy (infra/aws/iam/$p.json)"
done

# 2. the broad managed policies, all but IAMFullAccess
for p in AmazonSSMFullAccess AmazonEC2FullAccess AmazonVPCFullAccess AmazonS3FullAccess \
         AmazonRDSFullAccess CloudWatchFullAccessV2 AmazonSNSFullAccess AmazonRoute53FullAccess; do
  $AWS iam detach-user-policy --user-name onnet --policy-arn "arn:aws:iam::aws:policy/$p"
done

# 3. the old inline policies
for p in acm opennova-hosted-ops opennova-cost; do
  $AWS iam delete-user-policy --user-name onnet --policy-name "$p"
done

# 4. the five
for p in onnet-deploy-network onnet-deploy-compute onnet-deploy-iam onnet-deploy-data \
         onnet-deploy-observability; do
  $AWS iam attach-user-policy --user-name onnet --policy-arn "arn:aws:iam::310045376545:policy/$p"
done
$AWS iam list-attached-user-policies --user-name onnet   # the five and IAMFullAccess
$AWS iam list-user-policies --user-name onnet            # none

# 5. the last broad one
$AWS iam detach-user-policy --user-name onnet --policy-arn arn:aws:iam::aws:policy/IAMFullAccess
```

Between steps 2 and 4 `onnet` has less than either set; run nothing else meanwhile.

**After step 5 `onnet` can no longer change its own permissions** (nor read them). Any later
change to these policies, a rollback, or the cleanup of the dead statements needs an admin
principal (the account's root user or an admin role): `iam create-policy-version
--set-as-default` with the edited file, deleting the oldest version first once a policy has
five.

Checks after the switch, as `onnet`:

```bash
$AWS ec2 describe-vpcs --query 'Vpcs[].VpcId'                                          # works
$AWS s3api list-objects-v2 --bucket opennova-backups --prefix releases/ --max-items 1   # works
$AWS iam list-attached-user-policies --user-name onnet                                  # AccessDenied
$AWS s3api list-objects-v2 --bucket <another-project-bucket> --max-items 1                # AccessDenied
./deploy/run.sh infra plan                                                              # no AccessDenied
```

## To verify on the first applies

Checked against the Service Authorization Reference, provider v5.100.0's source and IAM's
policy simulator, but only a real apply proves these (each would show as an AccessDenied
naming the action):

- staging: the database's subnet group and instance (RDS's own checks of the subnets and
  groups; encrypted storage and Performance Insights on the AWS-managed `aws/rds` key, no KMS
  grant here); `AddRoleToInstanceProfile`'s PassRole check (`iam:PassedToService` is matched
  only if present); the first alarm with EC2 recover/reboot actions creating
  `AWSServiceRoleForCloudWatchEvents`; `app shell` and `db psql` (Session Manager document
  ARNs, `ssmmessages:OpenDataChannel`); a fleet launch and end under the role boundary.
- prod's cutover: the address moving from the old instance to the new interface, the old
  instance, launch templates, key pair, groups, roles, profiles and policies going, the
  budgets, the anomaly monitor and subscription (prod alone makes them; whether a create with
  tags also wants `ce:TagResource`, which is granted only on already-tagged monitors), and the
  account's S3 block planned as no change.
- the next `hosted data-snapshot`: `ebs:StartSnapshot` with its tags and no `ec2:CreateTags`.
- the first apply with `mail_sender`: `ses:CreateEmailIdentity` with the default tags
  (`ses:TagResource` is granted beside it), the MAIL FROM attributes, and the service's
  first send under the boundary (an AccessDenied in the `[mail]` log line).

## Rollback

With an admin principal: detach the five `onnet-deploy-*` policies from `onnet` and restore what
it had on 2026-10-15:

- managed: `AmazonSSMFullAccess`, `AmazonEC2FullAccess`, `IAMFullAccess`,
  `AmazonVPCFullAccess`, `AmazonS3FullAccess`, `AmazonRDSFullAccess`, `CloudWatchFullAccessV2`,
  `AmazonSNSFullAccess`, `AmazonRoute53FullAccess` (all `arn:aws:iam::aws:policy/<name>`);
- inline `acm`:
  `{"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":"acm:*","Resource":"*"},{"Effect":"Allow","Action":"ssm:*","Resource":"*"},{"Effect":"Allow","Action":"cloudfront:*","Resource":"*"}]}`
- inline `opennova-hosted-ops`:
  `{"Version":"2012-10-17","Statement":[{"Sid":"HostedDataSnapshot","Effect":"Allow","Action":["ebs:StartSnapshot","ebs:PutSnapshotBlock","ebs:CompleteSnapshot","kms:DescribeKey"],"Resource":"*"},{"Sid":"ReadQuotas","Effect":"Allow","Action":["servicequotas:GetServiceQuota","servicequotas:ListServiceQuotas"],"Resource":"*"},{"Sid":"DecodeAuthorizationMessages","Effect":"Allow","Action":["sts:DecodeAuthorizationMessage"],"Resource":"*"}]}`
- inline `opennova-cost`:
  `{"Version":"2012-10-17","Statement":[{"Sid":"Budgets","Effect":"Allow","Action":["budgets:*"],"Resource":"*"},{"Sid":"CostAnomalyDetection","Effect":"Allow","Action":["ce:CreateAnomalyMonitor","ce:UpdateAnomalyMonitor","ce:DeleteAnomalyMonitor","ce:GetAnomalyMonitors","ce:CreateAnomalySubscription","ce:UpdateAnomalySubscription","ce:DeleteAnomalySubscription","ce:GetAnomalySubscriptions","ce:GetAnomalies","ce:TagResource","ce:UntagResource","ce:ListTagsForResource"],"Resource":"*"},{"Sid":"CostAllocationTags","Effect":"Allow","Action":["ce:ListCostAllocationTags","ce:UpdateCostAllocationTagsStatus"],"Resource":"*"}]}`

The role boundary can stay: roles that carry it keep working.
