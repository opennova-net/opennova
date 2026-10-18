# ---------------------------------------------------------------------------
# The deploy workflow's role (.github/workflows/deploy.yml; DEPLOY.md, "The
# deploy workflow")
# ---------------------------------------------------------------------------
#
# GitHub's OIDC provider in the account, and opennova-<deployment>-github-deploy,
# which the workflow assumes for one run: trusted by the repository's jobs in
# its environment of the deployment's name alone (prod, whose required
# reviewers are the approval gate, or staging; infra/github's environments.tf),
# so a staging job never assumes prod's role nor the reverse. It does what
# `app deploy` and `app rollback` do and no more: read the deployment's
# infra.env and release, point the release, list which secrets SSM holds
# (names, never values), write and read its release manifests, register the
# release's revisions (handing them the stack's task roles), point the
# cluster's services at them and the fleet cluster's hostd, ask the service's
# admin API which hostd image each machine's agent runs (ECS Exec into this
# deployment's web container alone: `app deploy`'s check after hostd's roll),
# and point the nightly backup's schedule at the release's backup revision
# (handing it the schedule's role). It names
# infra/aws's resources by the names infra/aws gives them (opennova-<deployment>-*;
# a renamed one there is a change here too). It carries the role boundary.

locals {
  name      = "opennova-${var.deployment}"
  arn_ecs   = "arn:aws:ecs:${var.region}:${var.account_id}"
  arn_iam   = "arn:aws:iam::${var.account_id}"
  arn_ssm   = "arn:aws:ssm:${var.region}:${var.account_id}:parameter/opennova/${var.deployment}"
  ops_arn   = "arn:aws:s3:::${local.name}-ops-${var.account_id}"
  programs  = ["data", "fleet", "gate", "lobby", "web", "worker"]
  families  = concat([for p in local.programs : "${local.name}-${p}"], ["${local.name}-backup", "${local.name}-hostd"])
  task_role = concat(["${local.name}-execution", "${local.name}-backup", "${local.name}-hostd"], [for p in local.programs : "${local.name}-${p}"])
}

resource "aws_iam_openid_connect_provider" "github" {
  url            = "https://token.actions.githubusercontent.com"
  client_id_list = ["sts.amazonaws.com"]
  # GitHub's published thumbprints (AWS verifies GitHub's tokens against its own
  # trust store as well).
  thumbprint_list = ["6938fd4d98bab03faadb97b34396831e3780aea1", "1c58a3a8518e8759bf075b76b750d4f2df264fcd"]
}

resource "aws_iam_role" "github_deploy" {
  name                 = "${local.name}-github-deploy"
  description          = "The GitHub Actions deploy of the ${var.deployment} NovaWorld stack (.github/workflows/deploy.yml, environment ${var.deployment})"
  max_session_duration = 3600
  permissions_boundary = aws_iam_policy.boundary.arn

  assume_role_policy = jsonencode({
    Version = "2012-10-17"
    Statement = [{
      Sid       = "DeployWorkflowEnvironment"
      Effect    = "Allow"
      Principal = { Federated = aws_iam_openid_connect_provider.github.arn }
      Action    = "sts:AssumeRoleWithWebIdentity"
      Condition = {
        StringEquals = {
          "token.actions.githubusercontent.com:aud" = "sts.amazonaws.com"
          "token.actions.githubusercontent.com:sub" = "repo:${var.github_repository}:environment:${var.deployment}"
        }
      }
    }]
  })
}

resource "aws_iam_role_policy" "github_deploy" {
  name = "${local.name}-github-deploy"
  role = aws_iam_role.github_deploy.id
  policy = jsonencode({
    Version = "2012-10-17"
    Statement = [
      # The down marker too: a deploy tells `app down`'s zero counts from a first release by it.
      { Sid = "ReadInfraEnv", Effect = "Allow", Action = "ssm:GetParameter", Resource = ["${local.arn_ssm}/infra.env", "${local.arn_ssm}/release", "${local.arn_ssm}/down"] },
      { Sid = "PointRelease", Effect = "Allow", Action = "ssm:PutParameter", Resource = "${local.arn_ssm}/release" },
      # Names alone (DescribeParameters reads no value and takes no resource scope).
      { Sid = "ListSecretNames", Effect = "Allow", Action = "ssm:DescribeParameters", Resource = "*" },
      {
        Sid       = "ListReleases", Effect = "Allow", Action = "s3:ListBucket", Resource = local.ops_arn
        Condition = { StringLike = { "s3:prefix" = "releases/*" } }
      },
      { Sid = "WriteReleases", Effect = "Allow", Action = ["s3:PutObject", "s3:GetObject"], Resource = "${local.ops_arn}/releases/*" },
      # Reading and registering task definitions takes no resource scope.
      { Sid = "RegisterRevisions", Effect = "Allow", Action = ["ecs:DescribeTaskDefinition", "ecs:RegisterTaskDefinition"], Resource = "*" },
      {
        Sid       = "TagRevisions", Effect = "Allow", Action = "ecs:TagResource"
        Resource  = [for f in local.families : "${local.arn_ecs}:task-definition/${f}:*"]
        Condition = { StringEquals = { "ecs:CreateAction" = "RegisterTaskDefinition" } }
      },
      # The programs' services, and hostd, the fleet cluster's daemon.
      { Sid = "PointServices", Effect = "Allow", Action = ["ecs:DescribeServices", "ecs:UpdateService"], Resource = ["${local.arn_ecs}:service/${local.name}/*", "${local.arn_ecs}:service/${local.name}-fleet/hostd"] },
      # ECS Exec into a web task (deploy/bin/lib/app.sh, admin_api): its task
      # found (ListTasks takes the cluster as a condition alone) and described
      # (the CLI reads the container's runtime id), the exec into the web
      # container alone, and the session plugin's data channel (no resource
      # scope). ECS starts the session itself: no ssm:StartSession.
      {
        Sid       = "FindWebTask", Effect = "Allow", Action = "ecs:ListTasks", Resource = "*"
        Condition = { ArnEquals = { "ecs:cluster" = "${local.arn_ecs}:cluster/${local.name}" } }
      },
      { Sid = "DescribeTasks", Effect = "Allow", Action = "ecs:DescribeTasks", Resource = "${local.arn_ecs}:task/${local.name}/*" },
      {
        Sid       = "ExecIntoWeb", Effect = "Allow", Action = "ecs:ExecuteCommand"
        Resource  = ["${local.arn_ecs}:cluster/${local.name}", "${local.arn_ecs}:task/${local.name}/*"]
        Condition = { StringEquals = { "ecs:container-name" = "web" } }
      },
      { Sid = "ExecChannel", Effect = "Allow", Action = "ssmmessages:OpenDataChannel", Resource = "*" },
      {
        Sid       = "PassTaskRoles", Effect = "Allow", Action = "iam:PassRole"
        Resource  = [for r in local.task_role : "${local.arn_iam}:role/${r}"]
        Condition = { StringEquals = { "iam:PassedToService" = "ecs-tasks.amazonaws.com" } }
      },
      {
        Sid      = "PointBackupSchedule", Effect = "Allow", Action = ["scheduler:GetSchedule", "scheduler:UpdateSchedule"]
        Resource = "arn:aws:scheduler:${var.region}:${var.account_id}:schedule/default/${local.name}-backup"
      },
      {
        Sid       = "PassScheduleRole", Effect = "Allow", Action = "iam:PassRole"
        Resource  = "${local.arn_iam}:role/${local.name}-backup-schedule"
        Condition = { StringEquals = { "iam:PassedToService" = "scheduler.amazonaws.com" } }
      },
    ]
  })
}
