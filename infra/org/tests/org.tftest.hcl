# The org stack (organization.tf, accounts.tf, account/), planned against mock
# providers: offline, no credentials, nothing reaches AWS. From the repo root:
#   docker run --rm -v "$PWD:/w" -w /w/infra/org hashicorp/terraform:<the toolbox's> init -backend=false
#   docker run --rm -v "$PWD:/w" -w /w/infra/org hashicorp/terraform:<the toolbox's> test
# The organization and its three accounts adopted as they are (every feature,
# never closed; ci, the CI runners' account, with no baseline here: infra/ci);
# per deployment account, through its own provider: the state bucket
# named after the account, versioned, encrypted, private, TLS only and reached
# by its deploy user, its administrator role and its root alone; the deploy
# user with the six policies (each under IAM's 6,144 characters, naming its own
# account alone), its key, its state access (prod's the shared stacks' too,
# nobody's the org stack's) and its guard; the two boundaries (the role
# boundary, the machine boundary), each under 6,144 characters, which the deploy
# user may write a role under and no other; the GitHub deploy role trusted by
# the repository's environment of the deployment's name alone, under the role
# boundary, naming its own deployment's resources alone. And
# opennova-org's own policy: Organizations, Identity Center and the member
# accounts' administrator roles, nothing else. Identity Center: two permission
# sets, the maintainer, both sets in every member account, and no group.

mock_provider "aws" {
  override_during = plan

  # The organization's Identity Center instance, enabled by hand.
  mock_data "aws_ssoadmin_instances" {
    defaults = {
      arns               = ["arn:aws:sso:::instance/ssoins-0123456789abcdef"]
      identity_store_ids = ["d-0123456789"]
    }
  }
  mock_resource "aws_ssoadmin_permission_set" {
    defaults = { arn = "arn:aws:sso:::permissionSet/ssoins-0123456789abcdef/ps-0123456789abcdef" }
  }
  mock_resource "aws_identitystore_user" {
    defaults = { user_id = "90670000-0000-0000-0000-000000000000" }
  }
}

mock_provider "aws" {
  alias           = "prod"
  override_during = plan

  mock_resource "aws_s3_bucket" {
    defaults = { arn = "arn:aws:s3:::opennova-tfstate-111111111111" }
  }
  mock_resource "aws_iam_policy" {
    defaults = { arn = "arn:aws:iam::111111111111:policy/opennova-role-boundary" }
  }
  mock_resource "aws_iam_user" {
    defaults = { arn = "arn:aws:iam::111111111111:user/opennova-deploy" }
  }
  mock_resource "aws_iam_role" {
    defaults = { arn = "arn:aws:iam::111111111111:role/opennova-prod-github-deploy" }
  }
  mock_resource "aws_iam_openid_connect_provider" {
    defaults = { arn = "arn:aws:iam::111111111111:oidc-provider/token.actions.githubusercontent.com" }
  }
  mock_resource "aws_iam_access_key" {
    defaults = { id = "AKIAPRODMOCK", secret = "prod-secret-mock" }
  }
}

mock_provider "aws" {
  alias           = "staging"
  override_during = plan

  mock_resource "aws_s3_bucket" {
    defaults = { arn = "arn:aws:s3:::opennova-tfstate-222222222222" }
  }
  mock_resource "aws_iam_policy" {
    defaults = { arn = "arn:aws:iam::222222222222:policy/opennova-role-boundary" }
  }
  mock_resource "aws_iam_user" {
    defaults = { arn = "arn:aws:iam::222222222222:user/opennova-deploy" }
  }
  mock_resource "aws_iam_role" {
    defaults = { arn = "arn:aws:iam::222222222222:role/opennova-staging-github-deploy" }
  }
  mock_resource "aws_iam_openid_connect_provider" {
    defaults = { arn = "arn:aws:iam::222222222222:oidc-provider/token.actions.githubusercontent.com" }
  }
  mock_resource "aws_iam_access_key" {
    defaults = { id = "AKIASTAGINGMOCK", secret = "staging-secret-mock" }
  }
}

# The import blocks adopt these, and a mock provider cannot import: each is
# stood in for at plan instead.
override_resource {
  target          = aws_organizations_organization.org
  override_during = plan
}
override_resource {
  target          = aws_organizations_account.member["prod"]
  override_during = plan
  values          = { id = "111111111111" }
}
override_resource {
  target          = aws_organizations_account.member["staging"]
  override_during = plan
  values          = { id = "222222222222" }
}
override_resource {
  target          = aws_organizations_account.member["ci"]
  override_during = plan
  values          = { id = "444444444444" }
}

variables {
  region                 = "us-east-1"
  organization_id        = "o-abcdefghij"
  management_account_id  = "999999999999"
  aws_account_id_prod    = "111111111111"
  aws_account_id_staging = "222222222222"
  aws_account_id_ci      = "444444444444"
  account_name_prod      = "opennova-prod"
  account_name_staging   = "opennova-staging"
  account_name_ci        = "opennova-ci"
  root_email_prod        = "aws-prod@example.com"
  root_email_staging     = "aws-staging@example.com"
  root_email_ci          = "aws-ci@example.com"
  sso_given_name         = "Mock"
  sso_family_name        = "Maintainer"
  sso_email              = "maintainer@mail.example.org"
}

run "organization_and_accounts" {
  command = plan

  assert {
    condition     = aws_organizations_organization.org.feature_set == "ALL"
    error_message = "the organization keeps every feature"
  }
  assert {
    condition = (
      aws_organizations_account.member["prod"].name == "opennova-prod" &&
      aws_organizations_account.member["prod"].email == "aws-prod@example.com" &&
      aws_organizations_account.member["staging"].name == "opennova-staging" &&
      aws_organizations_account.member["staging"].email == "aws-staging@example.com" &&
      aws_organizations_account.member["ci"].name == "opennova-ci" &&
      aws_organizations_account.member["ci"].email == "aws-ci@example.com"
    )
    error_message = "each account carries the vault's name and root address"
  }
  assert {
    condition     = alltrue([for a in aws_organizations_account.member : !a.close_on_deletion])
    error_message = "an account leaving the stack is never closed"
  }
  assert {
    condition     = aws_organizations_account.member["prod"].tags["opennova:deployment"] == "opennova-prod" && aws_organizations_account.member["staging"].tags["opennova:deployment"] == "opennova-staging" && aws_organizations_account.member["ci"].tags["opennova:deployment"] == "opennova-ci"
    error_message = "each account is tagged with its deployment"
  }
  assert {
    condition     = toset(keys(output.deployments)) == toset(["prod", "staging"])
    error_message = "the ci account holds no deployment: no state bucket, deploy user or deploy role of its own"
  }
  assert {
    condition     = output.deployments.prod.account_id == "111111111111" && output.deployments.staging.account_id == "222222222222"
    error_message = "the outputs name each deployment's account"
  }
}

run "state_buckets" {
  command = plan

  assert {
    condition     = output.deployments.prod.state_bucket == "opennova-tfstate-111111111111" && output.deployments.staging.state_bucket == "opennova-tfstate-222222222222"
    error_message = "each account's state bucket is opennova-tfstate-<its account id>"
  }
  assert {
    condition = alltrue([for d in [module.prod.documents.bucket, module.staging.documents.bucket] :
      length(jsondecode(d).Statement) == 2
      && jsondecode(d).Statement[0].Effect == "Deny"
      && jsondecode(d).Statement[0].Condition.Bool["aws:SecureTransport"] == "false"
    && jsondecode(d).Statement[1].Effect == "Deny"])
    error_message = "a state bucket refuses every request not over TLS, and every principal but its own"
  }
  assert {
    condition = toset(jsondecode(module.prod.documents.bucket).Statement[1].Condition.ArnNotEquals["aws:PrincipalArn"]) == toset([
      "arn:aws:iam::111111111111:user/opennova-deploy",
      "arn:aws:iam::111111111111:role/OrganizationAccountAccessRole",
      "arn:aws:iam::111111111111:root",
    ])
    error_message = "prod's state bucket admits prod's deploy user, its administrator role and its root alone"
  }
  assert {
    condition     = !strcontains(module.staging.documents.bucket, "111111111111") && !strcontains(module.prod.documents.bucket, "222222222222")
    error_message = "no account's state bucket admits another account's principal"
  }
}

run "deploy_users" {
  command = plan

  assert {
    condition     = output.deployments.prod.deploy_user_arn == "arn:aws:iam::111111111111:user/opennova-deploy" && output.deployments.staging.deploy_user_arn == "arn:aws:iam::222222222222:user/opennova-deploy"
    error_message = "each account has its own deploy user"
  }
  assert {
    condition = (
      nonsensitive(output.deploy_keys.prod.access_key_id) == "AKIAPRODMOCK" &&
      nonsensitive(output.deploy_keys.staging.access_key_id) == "AKIASTAGINGMOCK" &&
      nonsensitive(output.deploy_keys.prod.secret_access_key) == "prod-secret-mock"
    )
    error_message = "each deploy user's key reaches the toolbox through the sensitive output deploy_keys"
  }
  assert {
    condition     = toset(keys(module.prod.documents.deploy)) == toset(["network", "compute", "ecs", "iam", "data", "observability"])
    error_message = "the deploy user carries the six opennova-deploy-* policies"
  }
  assert {
    condition = alltrue(flatten([for m in [module.prod.documents, module.staging.documents] :
    [for d in concat(values(m.deploy), [m.boundary, m.machine_boundary]) : length(d) < 6144 && !strcontains(d, "$${") && !strcontains(d, "LegacyStack")]]))
    error_message = "each managed policy is filled, under IAM's 6,144 characters, with no statement of the old single-account stack"
  }
  assert {
    condition = alltrue([for d in concat(values(module.prod.documents.deploy), [module.prod.documents.boundary, module.prod.documents.machine_boundary]) :
    length(regexall("arn:aws:[a-z0-9-]+:[a-z0-9-]*:[0-9]{12}:", d)) == length(regexall("arn:aws:[a-z0-9-]+:[a-z0-9-]*:111111111111:", d))])
    error_message = "prod's policies name prod's account and no other"
  }
  assert {
    condition = alltrue([for d in concat(values(module.staging.documents.deploy), [module.staging.documents.boundary, module.staging.documents.machine_boundary]) :
    length(regexall("arn:aws:[a-z0-9-]+:[a-z0-9-]*:[0-9]{12}:", d)) == length(regexall("arn:aws:[a-z0-9-]+:[a-z0-9-]*:222222222222:", d))])
    error_message = "staging's policies name staging's account and no other"
  }
  assert {
    condition     = strcontains(module.prod.documents.boundary, "scheduler:UpdateSchedule") && strcontains(module.prod.documents.boundary, "scheduler.amazonaws.com")
    error_message = "the boundary lets the deploy workflow's role point the backup schedule and hand it its role"
  }
  assert {
    condition = alltrue([
      for st in jsondecode(module.prod.documents.boundary).Statement : (
        st.Sid == "ListTasks" ? (
          st.Action == "ecs:ListTasks" && st.Resource == "*" &&
          st.Condition.ArnLike["ecs:cluster"] == "arn:aws:ecs:us-east-1:111111111111:cluster/opennova-*"
          ) : st.Sid == "TasksDbCache" ? (
          toset(st.Action) == toset(["ecs:DescribeTasks", "ecs:ExecuteCommand", "rds:DescribeDBInstances", "elasticache:DescribeReplicationGroups"]) &&
          toset(st.Resource) == toset([
            "arn:aws:ecs:us-east-1:111111111111:task/opennova-*/*",
            "arn:aws:rds:us-east-1:111111111111:db:opennova-*",
            "arn:aws:elasticache:us-east-1:111111111111:replicationgroup:opennova-*",
          ])
          ) : st.Sid == "Clusters" ? (
          toset(st.Action) == toset(["ecs:ListContainerInstances", "ecs:ExecuteCommand"]) &&
          st.Resource == "arn:aws:ecs:us-east-1:111111111111:cluster/opennova-*"
          ) : st.Sid == "ReadFleet" ? (
          st.Action == "ecs:DescribeContainerInstances" && st.Resource == "arn:aws:ecs:us-east-1:111111111111:container-instance/opennova-*/*"
          ) : st.Sid == "Unscoped" ? (
          contains(st.Action, "elasticloadbalancing:DescribeTargetHealth") && contains(st.Action, "cloudwatch:GetMetricData") &&
          contains(st.Action, "cloudwatch:DescribeAlarms") && contains(st.Action, "ce:GetCostAndUsage") &&
          contains(st.Action, "ce:GetCostForecast") && contains(st.Action, "budgets:ViewBudget") &&
          st.Resource == "*" && !contains(keys(st), "Condition")
          ) : st.Sid == "Logs" ? (
          toset(st.Action) == toset(["logs:CreateLogStream", "logs:PutLogEvents", "logs:DescribeLogStreams", "logs:GetLogEvents", "logs:FilterLogEvents"]) &&
          toset(st.Resource) == toset([
            "arn:aws:logs:us-east-1:111111111111:log-group:/opennova/*",
            "arn:aws:logs:us-east-1:111111111111:log-group:/opennova/*:log-stream:*",
          ]) && !contains(keys(st), "Condition")
        ) : true
      )
      ]) && length([for st in jsondecode(module.prod.documents.boundary).Statement : st
    if contains(["ListTasks", "TasksDbCache", "Clusters", "ReadFleet", "Unscoped", "Logs"], st.Sid)]) == 6
    error_message = "the boundary holds web's infrastructure reads (infra/aws/programs.tf, fleet_agent.tf): OpenNova's clusters' tasks and container instances, its database and cache, the target groups' health, CloudWatch's metrics and alarms, Cost Explorer's month and forecast and the monthly budget (web's costs view), and the /opennova/* logs' events; and the deploy workflow's ECS Exec on OpenNova's clusters and tasks"
  }
  assert {
    condition = alltrue([for m in [module.prod.documents, module.staging.documents] :
      length([for st in jsondecode(m.boundary).Statement : st if st.Resource == "*" && !contains(keys(st), "Condition")]) == 1 &&
      length(jsondecode(m.boundary).Statement) == length(distinct([for st in jsondecode(m.boundary).Statement : st.Sid])) &&
      !strcontains(m.boundary, "ecs:RegisterContainerInstance") && !strcontains(m.boundary, "ecs:Poll") && !strcontains(m.boundary, "ecs:DiscoverPollEndpoint")
    ])
    error_message = "the role boundary holds its unscoped actions in one statement, names each statement once and holds none of the machines' ECS agent calls (the machine boundary's)"
  }
  assert {
    condition = alltrue([
      for st in jsondecode(module.prod.documents.boundary).Statement : (
        st.Sid == "LaunchTagged" ? (
          toset(st.Action) == toset(["ec2:RunInstances", "ec2:AllocateAddress"]) &&
          toset(st.Resource) == toset(["arn:aws:ec2:us-east-1:111111111111:instance/*", "arn:aws:ec2:us-east-1:111111111111:elastic-ip/*"]) &&
          st.Condition == { StringEquals = { "aws:RequestTag/opennova:hosted" = "1" }, StringLike = { "aws:RequestTag/opennova:deployment" = "opennova-*" } }
          ) : st.Sid == "TagAtLaunch" ? (
          st.Action == "ec2:CreateTags" && toset(st.Condition.StringEquals["ec2:CreateAction"]) == toset(["RunInstances", "AllocateAddress"]) &&
          length(keys(st.Condition)) == 1 && length(keys(st.Condition.StringEquals)) == 1
          ) : st.Sid == "EndMachines" ? (
          toset(st.Action) == toset(["ec2:TerminateInstances", "ec2:AssociateAddress"]) &&
          st.Resource == "arn:aws:ec2:us-east-1:111111111111:instance/*" &&
          st.Condition == { StringEquals = { "aws:ResourceTag/opennova:hosted" = "1" }, StringLike = { "aws:ResourceTag/opennova:deployment" = "opennova-*" } }
          ) : st.Sid == "MoveAddresses" ? (
          toset(st.Action) == toset(["ec2:AssociateAddress", "ec2:DisassociateAddress", "ec2:ReleaseAddress"]) &&
          st.Resource == "arn:aws:ec2:us-east-1:111111111111:elastic-ip/*" &&
          st.Condition == { StringEquals = { "aws:ResourceTag/opennova:hosted" = "1" }, StringLike = { "aws:ResourceTag/opennova:deployment" = "opennova-*" } }
          ) : st.Sid == "PlayRecords" ? (
          st.Action == "route53:ChangeResourceRecordSets" && st.Resource == "arn:aws:route53:::hostedzone/*" &&
          st.Condition["ForAllValues:StringEquals"]["route53:ChangeResourceRecordSetsRecordTypes"] == "A" &&
          toset(st.Condition["ForAllValues:StringEquals"]["route53:ChangeResourceRecordSetsActions"]) == toset(["UPSERT", "DELETE"]) &&
          st.Condition["ForAllValues:StringLike"] == { "route53:ChangeResourceRecordSetsNormalizedRecordNames" = "*.play.*" }
          ) : st.Sid == "ZoneChanges" ? (
          st.Action == "route53:GetChange" && st.Resource == "arn:aws:route53:::change/*" && !contains(keys(st), "Condition")
          ) : st.Sid == "Unscoped" ? (
          contains(st.Action, "ec2:DescribeAddresses")
        ) : !strcontains(jsonencode(st), "Address") && !strcontains(jsonencode(st), "route53:")
      )
      ]) && length([for st in jsondecode(module.prod.documents.boundary).Statement : st
    if contains(["LaunchTagged", "TagAtLaunch", "EndMachines", "MoveAddresses", "PlayRecords", "ZoneChanges"], st.Sid)]) == 6
    error_message = "the role boundary holds the worker's static addresses: allocated and tagged at creation under this deployment's tags, associated with its machines, moved and released only while they carry both tags, their A records under a play zone by UPSERT and DELETE alone, a change's state; nothing else names an address or Route 53"
  }
  assert {
    condition = alltrue([
      for st in jsondecode(module.prod.documents.machine_boundary).Statement : (
        st.Sid == "EcsAgentCluster" ? (
          toset(st.Action) == toset([
            "ecs:RegisterContainerInstance", "ecs:DeregisterContainerInstance", "ecs:SubmitContainerStateChange",
            "ecs:SubmitTaskStateChange", "ecs:SubmitAttachmentStateChanges",
          ]) && st.Resource == "arn:aws:ecs:us-east-1:111111111111:cluster/opennova-*-fleet"
          ) : st.Sid == "EcsAgentInstance" ? (
          toset(st.Action) == toset(["ecs:Poll", "ecs:StartTelemetrySession"]) &&
          st.Resource == "arn:aws:ecs:us-east-1:111111111111:container-instance/opennova-*-fleet/*"
          ) : st.Sid == "Unscoped" ? (
          toset(st.Action) == toset([
            "ecs:DiscoverPollEndpoint", "ssmmessages:CreateControlChannel", "ssmmessages:CreateDataChannel",
            "ssmmessages:OpenControlChannel", "ssmmessages:OpenDataChannel",
          ]) && st.Resource == "*" && !contains(keys(st), "Condition")
          ) : st.Sid == "MachineAndServerLogs" ? (
          toset(st.Action) == toset(["logs:CreateLogStream", "logs:PutLogEvents"]) &&
          toset(st.Resource) == toset([
            "arn:aws:logs:us-east-1:111111111111:log-group:/opennova/*/machines",
            "arn:aws:logs:us-east-1:111111111111:log-group:/opennova/*/machines:*",
            "arn:aws:logs:us-east-1:111111111111:log-group:/opennova/*/servers",
            "arn:aws:logs:us-east-1:111111111111:log-group:/opennova/*/servers:*",
          ])
          ) : st.Sid == "ListTrees" ? (
          st.Action == "s3:ListBucket" && st.Resource == "arn:aws:s3:::opennova-*" &&
          st.Condition == { StringLike = { "s3:prefix" = "data/trees/*" } }
          ) : st.Sid == "ReadTrees" ? (
          st.Action == "s3:GetObject" && st.Resource == "arn:aws:s3:::opennova-*/data/trees/*"
        ) : false
      )
    ]) && length(jsondecode(module.prod.documents.machine_boundary).Statement) == 6
    error_message = "the machine boundary holds the ECS agent's calls on the fleet clusters and their instances, ECS Exec's channels, the machines' and servers' logs and the trees' list and read, nothing else"
  }
  assert {
    condition = alltrue([for account, m in { "111111111111" = module.prod.documents, "222222222222" = module.staging.documents } :
      toset(one([for st in jsondecode(m.deploy["iam"]).Statement : st.Condition.StringEquals["iam:PermissionsBoundary"] if st.Sid == "WriteRolesUnderTheBoundary"])) == toset([
        "arn:aws:iam::${account}:policy/opennova-role-boundary",
        "arn:aws:iam::${account}:policy/opennova-machine-boundary",
      ])
    ])
    error_message = "the deploy user writes a role under the role boundary or the machine boundary, and no other"
  }
  assert {
    condition = (
      strcontains(module.prod.documents.deploy["network"], "ec2:AcceptAddressTransfer") &&
      strcontains(module.prod.documents.deploy["network"], "\"aws:ResourceTag/opennova:deployment\":\"opennova-prod\"")
    )
    error_message = "the deploy user accepts the gate's transferred address, and releases no address tagged prod's"
  }
}

run "state_access" {
  command = plan

  assert {
    condition = (
      toset(one([for s in jsondecode(module.prod.documents.state).Statement : s.Resource if s.Sid == "StateObjects"])) == toset([
        "arn:aws:s3:::opennova-tfstate-111111111111/infra/aws/*",
        "arn:aws:s3:::opennova-tfstate-111111111111/infra/zone/*",
        "arn:aws:s3:::opennova-tfstate-111111111111/infra/github/*",
      ]) &&
      toset(one([for s in jsondecode(module.prod.documents.state).Statement : s.Resource if s.Sid == "StateLocks"])) == toset([
        "arn:aws:s3:::opennova-tfstate-111111111111/infra/aws/terraform.tfstate.tflock",
        "arn:aws:s3:::opennova-tfstate-111111111111/infra/zone/terraform.tfstate.tflock",
        "arn:aws:s3:::opennova-tfstate-111111111111/infra/github/terraform.tfstate.tflock",
      ])
    )
    error_message = "prod's deploy user runs infra/aws and the shared stacks (zone, github): their states and their lock files alone"
  }
  assert {
    condition = (
      one([for s in jsondecode(module.staging.documents.state).Statement : s.Resource if s.Sid == "StateObjects"]) == ["arn:aws:s3:::opennova-tfstate-222222222222/infra/aws/*"] &&
      one([for s in jsondecode(module.staging.documents.state).Statement : s.Resource if s.Sid == "StateLocks"]) == ["arn:aws:s3:::opennova-tfstate-222222222222/infra/aws/terraform.tfstate.tflock"]
    )
    error_message = "staging's deploy user runs staging's infra/aws alone"
  }
  assert {
    condition     = !strcontains(module.prod.documents.state, "/org/") && !strcontains(module.prod.documents.state, "DeleteObjectVersion")
    error_message = "no deploy user reaches the org stack's state, nor deletes a state's old version"
  }
  assert {
    condition = alltrue([for m in [module.prod.documents, module.staging.documents] :
      toset(jsondecode(m.guard).Statement[*].Effect) == toset(["Deny"])
    && contains(jsondecode(m.guard).Statement[0].Action, "s3:DeleteObjectVersion")])
    error_message = "the guard keeps each deploy user off its state bucket's settings and old versions"
  }
}

run "github_deploy_roles" {
  command = plan

  assert {
    condition = (
      jsondecode(module.prod.documents.github_trust).Statement[0].Action == "sts:AssumeRoleWithWebIdentity" &&
      jsondecode(module.prod.documents.github_trust).Statement[0].Principal == { Federated = "arn:aws:iam::111111111111:oidc-provider/token.actions.githubusercontent.com" } &&
      jsondecode(module.prod.documents.github_trust).Statement[0].Condition.StringEquals["token.actions.githubusercontent.com:sub"] == "repo:opennova-net/opennova:environment:prod" &&
      jsondecode(module.prod.documents.github_trust).Statement[0].Condition.StringEquals["token.actions.githubusercontent.com:aud"] == "sts.amazonaws.com"
    )
    error_message = "prod's GitHub deploy role trusts the repository's prod environment alone, through prod's own OIDC provider"
  }
  assert {
    condition     = jsondecode(module.staging.documents.github_trust).Statement[0].Condition.StringEquals["token.actions.githubusercontent.com:sub"] == "repo:opennova-net/opennova:environment:staging"
    error_message = "staging's GitHub deploy role trusts the repository's staging environment alone"
  }
  assert {
    condition     = module.prod.documents.github_boundary == "arn:aws:iam::111111111111:policy/opennova-role-boundary"
    error_message = "the GitHub deploy role carries the role boundary"
  }
  assert {
    condition = (
      toset(one([for s in jsondecode(module.prod.documents.github).Statement : s.Resource if s.Sid == "PassTaskRoles"])) == toset([
        "arn:aws:iam::111111111111:role/opennova-prod-execution",
        "arn:aws:iam::111111111111:role/opennova-prod-backup",
        "arn:aws:iam::111111111111:role/opennova-prod-data",
        "arn:aws:iam::111111111111:role/opennova-prod-fleet",
        "arn:aws:iam::111111111111:role/opennova-prod-gate",
        "arn:aws:iam::111111111111:role/opennova-prod-hostd",
        "arn:aws:iam::111111111111:role/opennova-prod-lobby",
        "arn:aws:iam::111111111111:role/opennova-prod-web",
        "arn:aws:iam::111111111111:role/opennova-prod-worker",
      ]) &&
      toset(one([for s in jsondecode(module.prod.documents.github).Statement : s.Resource if s.Sid == "PointServices"])) == toset([
        "arn:aws:ecs:us-east-1:111111111111:service/opennova-prod/*",
        "arn:aws:ecs:us-east-1:111111111111:service/opennova-prod-fleet/hostd",
      ]) &&
      contains(one([for s in jsondecode(module.prod.documents.github).Statement : s.Resource if s.Sid == "TagRevisions"]), "arn:aws:ecs:us-east-1:111111111111:task-definition/opennova-prod-hostd:*") &&
      one([for s in jsondecode(module.prod.documents.github).Statement : s.Resource if s.Sid == "WriteReleases"]) == "arn:aws:s3:::opennova-prod-ops-111111111111/releases/*"
    )
    error_message = "prod's GitHub deploy role hands ECS prod's task roles alone (hostd's among them), points prod's services and its fleet cluster's hostd and writes prod's releases"
  }
  assert {
    condition = toset(one([for s in jsondecode(module.prod.documents.github).Statement : s.Resource if s.Sid == "ReadInfraEnv"])) == toset([
      "arn:aws:ssm:us-east-1:111111111111:parameter/opennova/prod/infra.env",
      "arn:aws:ssm:us-east-1:111111111111:parameter/opennova/prod/release",
      "arn:aws:ssm:us-east-1:111111111111:parameter/opennova/prod/down",
    ])
    error_message = "prod's GitHub deploy role reads its infra.env, its release and its down marker (a deploy tells app down's zero counts from a first release by it), nothing else"
  }
  assert {
    condition     = !strcontains(module.staging.documents.github, "opennova-prod") && !strcontains(module.staging.documents.github, "111111111111")
    error_message = "staging's GitHub deploy role names nothing of prod's"
  }
  assert {
    condition = (
      one([for s in jsondecode(module.prod.documents.github).Statement : s.Action if s.Sid == "ExecIntoWeb"]) == "ecs:ExecuteCommand" &&
      one([for s in jsondecode(module.prod.documents.github).Statement : s.Resource if s.Sid == "ExecIntoWeb"]) == [
        "arn:aws:ecs:us-east-1:111111111111:cluster/opennova-prod",
        "arn:aws:ecs:us-east-1:111111111111:task/opennova-prod/*",
      ] &&
      one([for s in jsondecode(module.prod.documents.github).Statement : s.Condition if s.Sid == "ExecIntoWeb"]) == { StringEquals = { "ecs:container-name" = "web" } } &&
      length([for s in jsondecode(module.prod.documents.github).Statement : s if contains(flatten([s.Action]), "ecs:ExecuteCommand")]) == 1 &&
      one([for s in jsondecode(module.prod.documents.github).Statement : s.Action if s.Sid == "FindWebTask"]) == "ecs:ListTasks" &&
      one([for s in jsondecode(module.prod.documents.github).Statement : s.Condition if s.Sid == "FindWebTask"]) == { ArnEquals = { "ecs:cluster" = "arn:aws:ecs:us-east-1:111111111111:cluster/opennova-prod" } } &&
      one([for s in jsondecode(module.prod.documents.github).Statement : s.Resource if s.Sid == "DescribeTasks"]) == "arn:aws:ecs:us-east-1:111111111111:task/opennova-prod/*" &&
      one([for s in jsondecode(module.prod.documents.github).Statement : s.Action if s.Sid == "ExecChannel"]) == "ssmmessages:OpenDataChannel" &&
      !strcontains(module.prod.documents.github, "ssm:StartSession")
    )
    error_message = "prod's GitHub deploy role execs into prod's web container alone (app deploy's check of the machines' hostd image), finding and describing prod's tasks first, and starts no Session Manager session of its own"
  }
  assert {
    condition     = output.deployments.prod.github_deploy_role_arn == "arn:aws:iam::111111111111:role/opennova-prod-github-deploy"
    error_message = "the role's ARN reaches the vault (aws-prod/github_deploy_role_arn) through the outputs"
  }
}

run "org_user_policy" {
  command = plan

  assert {
    condition = (
      length(output.org_user_policy) < 6144 &&
      toset(flatten([for s in jsondecode(output.org_user_policy).Statement : s.Action if !startswith(s.Sid, "IdentityCenter")])) == toset([
        "organizations:Describe*", "organizations:List*", "organizations:CreateAccount",
        "organizations:TagResource", "organizations:UntagResource", "sts:AssumeRole",
      ]) &&
      toset(one([for s in jsondecode(output.org_user_policy).Statement : s.Action if s.Sid == "IdentityCenter"])) == toset(["sso:*", "identitystore:*"]) &&
      toset(one([for s in jsondecode(output.org_user_policy).Statement : s.Resource if s.Sid == "IdentityCenterRolesAndProviders"])) == toset([
        "arn:aws:iam::*:role/aws-reserved/sso.amazonaws.com/*", "arn:aws:iam::*:saml-provider/AWSSSO_*",
      ])
    )
    error_message = "opennova-org's policy renders, and allows Organizations, Identity Center (its own roles and providers alone in IAM) and the member accounts' administrator roles, nothing else"
  }
  assert {
    condition = one([for s in jsondecode(output.org_user_policy).Statement : s.Resource if s.Sid == "MemberAccountAdministrators"]) == [
      "arn:aws:iam::111111111111:role/OrganizationAccountAccessRole",
      "arn:aws:iam::222222222222:role/OrganizationAccountAccessRole",
      "arn:aws:iam::444444444444:role/OrganizationAccountAccessRole",
    ]
    error_message = "opennova-org assumes the member accounts' OrganizationAccountAccessRole alone"
  }
}

run "identity_center" {
  command = plan

  assert {
    condition = (
      aws_ssoadmin_permission_set.set["admin"].name == "OpenNovaAdmin" && aws_ssoadmin_permission_set.set["admin"].session_duration == "PT4H" &&
      aws_ssoadmin_permission_set.set["readonly"].name == "OpenNovaReadOnly" &&
      aws_ssoadmin_permission_set.set["admin"].instance_arn == "arn:aws:sso:::instance/ssoins-0123456789abcdef"
    )
    error_message = "two permission sets in the organization's instance: OpenNovaAdmin (4-hour sessions) and OpenNovaReadOnly"
  }
  assert {
    condition = (
      aws_ssoadmin_managed_policy_attachment.set["admin"].managed_policy_arn == "arn:aws:iam::aws:policy/AdministratorAccess" &&
      aws_ssoadmin_managed_policy_attachment.set["readonly"].managed_policy_arn == "arn:aws:iam::aws:policy/ReadOnlyAccess"
    )
    error_message = "the sets carry AdministratorAccess and ReadOnlyAccess"
  }
  assert {
    condition = (
      aws_identitystore_user.maintainer.identity_store_id == "d-0123456789" &&
      aws_identitystore_user.maintainer.user_name == "maintainer@mail.example.org" &&
      aws_identitystore_user.maintainer.name[0].given_name == "Mock" &&
      aws_identitystore_user.maintainer.name[0].family_name == "Maintainer" &&
      aws_identitystore_user.maintainer.emails[0].value == "maintainer@mail.example.org"
    )
    error_message = "one user, the maintainer, with the vault's name and address"
  }
  assert {
    condition = (
      toset(keys(aws_ssoadmin_account_assignment.maintainer)) == toset(["admin/prod", "admin/staging", "admin/ci", "readonly/prod", "readonly/staging", "readonly/ci"]) &&
      alltrue([for a in aws_ssoadmin_account_assignment.maintainer : a.principal_type == "USER" && a.target_type == "AWS_ACCOUNT" && a.principal_id == "90670000-0000-0000-0000-000000000000"]) &&
      aws_ssoadmin_account_assignment.maintainer["admin/prod"].target_id == "111111111111" &&
      aws_ssoadmin_account_assignment.maintainer["readonly/staging"].target_id == "222222222222" &&
      aws_ssoadmin_account_assignment.maintainer["admin/ci"].target_id == "444444444444"
    )
    error_message = "both sets are the maintainer's in every member account, assigned to the user (no group)"
  }
  assert {
    condition     = output.sso_portal_url == "https://d-0123456789.awsapps.com/start"
    error_message = "the portal's default address reaches the vault (aws-org/sso_portal_url)"
  }
}

run "management_account_must_be_an_id" {
  command = plan

  variables {
    management_account_id = "REPLACE_ME"
  }

  expect_failures = [var.management_account_id]
}

run "root_email_must_be_an_address" {
  command = plan

  variables {
    root_email_staging = "aws-staging"
  }

  expect_failures = [var.root_email_staging]
}
