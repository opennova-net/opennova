# ---------------------------------------------------------------------------
# The database's dumps: a scheduled ECS task (DEPLOY.md, section 5)
# ---------------------------------------------------------------------------
#
# The backup image (deploy/backup, <image_registry>/opennova-novaworld-backup):
# pg_dump -Fc of the database to the deployment's operations bucket
# (ops_bucket.tf) under BACKUP_PREFIX (novaworld/), kept 90 days,
# independent of the RDS instance (whose own point-in-time recovery covers 14
# days). EventBridge Scheduler runs it nightly at 03:00 UTC as a Fargate task
# of the release's backup revision (a deploy registers one with the release's
# image by digest, as it does each program's, and points the schedule at it;
# a rollback points it back): never the family's newest revision, which an
# infra apply's template (its image `latest`) may be. Before the first release
# nothing runs and the schedule is off. The toolbox runs the same
# task definition for its one-off work, a command each: `backup now`, `backup
# restore`, `db bootstrap`, `db psql`, `db reset-db` (its image carries psql,
# pg_dump and the AWS CLI, and pins the RDS CA bundle at
# /app/rds-global-bundle.pem, where DATABASE_URL's sslrootcert names it).
# Its task role writes and reads the deployment's dumps alone (nothing deletes
# one: the bucket's lifecycle ends them) and reads the master password `db`
# commands lend it for their run.

resource "aws_security_group" "backup" {
  name        = "${local.name}-backup"
  description = "OpenNova NovaWorld backup and one-off tasks: nothing inbound"
  vpc_id      = aws_vpc.main.id

  tags = merge(var.tags, {
    Name = "${local.name}-backup"
  })
}

resource "aws_vpc_security_group_egress_rule" "backup_all" {
  security_group_id = aws_security_group.backup.id
  description       = "All outbound"
  ip_protocol       = "-1"
  cidr_ipv4         = "0.0.0.0/0"
}

# ---- its task role ------------------------------------------------------------------

resource "aws_iam_role" "backup" {
  name                 = "${local.name}-backup"
  assume_role_policy   = data.aws_iam_policy_document.ecs_tasks_assume_role.json
  permissions_boundary = local.role_boundary_arn

  tags = merge(var.tags, {
    Name = "${local.name}-backup"
  })
}

data "aws_iam_policy_document" "backup" {
  statement {
    sid       = "ListDumps"
    effect    = "Allow"
    actions   = ["s3:ListBucket"]
    resources = [local.ops_bucket_arn]

    condition {
      test     = "StringLike"
      variable = "s3:prefix"
      values   = ["${local.backup_prefix}/*"]
    }
  }

  statement {
    sid       = "WriteAndReadDumps"
    effect    = "Allow"
    actions   = ["s3:PutObject", "s3:GetObject", "s3:AbortMultipartUpload"]
    resources = ["${local.ops_bucket_arn}/${local.backup_prefix}/*"]
  }

  # `db bootstrap` and `db reset-db` lend the master password for their run
  # (the toolbox writes it, the task reads it, the toolbox deletes it).
  statement {
    sid       = "ReadLentMasterPassword"
    effect    = "Allow"
    actions   = ["ssm:GetParameter"]
    resources = ["${local.ssm_parameter_prefix}/bootstrap/db-master-password"]
  }
}

resource "aws_iam_role_policy" "backup" {
  name   = "${local.name}-backup"
  role   = aws_iam_role.backup.id
  policy = data.aws_iam_policy_document.backup.json
}

# `db psql` without SQL is an interactive psql in a one-off task, over ECS Exec.
resource "aws_iam_role_policy" "backup_exec" {
  name   = "${local.name}-backup-exec"
  role   = aws_iam_role.backup.id
  policy = data.aws_iam_policy_document.exec.json
}

# ---- its task definition ---------------------------------------------------------------

locals {
  backup_family = "${local.name}-backup"

  backup_container = {
    name      = "backup"
    image     = "${var.image_registry}/${local.backup_image}:${var.image_tag}"
    essential = true
    # One dump (the toolbox's one-off tasks give a command of their own).
    command = ["/backup/backup.sh"]
    environment = [for n, v in {
      DATABASE_URL       = local.database_url
      BACKUP_BUCKET      = local.ops_bucket_name
      BACKUP_PREFIX      = local.backup_prefix
      AWS_REGION         = local.region
      AWS_DEFAULT_REGION = local.region
    } : { name = n, value = v }]
    secrets         = [{ name = "DATABASE_PASSWORD", valueFrom = "${local.ssm_parameter_prefix}/secret/DATABASE_PASSWORD" }]
    linuxParameters = { initProcessEnabled = true }
    logConfiguration = {
      logDriver = "awslogs"
      options   = merge(local.awslogs, { "awslogs-stream-prefix" = "backup" })
    }
  }
}

resource "aws_ecs_task_definition" "backup" {
  family                   = local.backup_family
  requires_compatibilities = ["FARGATE"]
  network_mode             = "awsvpc"
  cpu                      = "256"
  memory                   = "512"
  execution_role_arn       = aws_iam_role.execution.arn
  task_role_arn            = aws_iam_role.backup.arn
  container_definitions    = jsonencode([local.backup_container])

  runtime_platform {
    operating_system_family = "LINUX"
    cpu_architecture        = "X86_64"
  }

  tags = merge(var.tags, {
    Name = local.backup_family
  })
}

# ---- the nightly run ---------------------------------------------------------------------

# EventBridge Scheduler's role: run the backup task definition (any of its
# revisions: the schedule names the release's) in this cluster alone, pass it
# its two roles, and tag the task it starts.
data "aws_iam_policy_document" "scheduler_assume_role" {
  statement {
    effect  = "Allow"
    actions = ["sts:AssumeRole"]

    principals {
      type        = "Service"
      identifiers = ["scheduler.amazonaws.com"]
    }

    condition {
      test     = "StringEquals"
      variable = "aws:SourceAccount"
      values   = [local.account_id]
    }
  }
}

resource "aws_iam_role" "backup_schedule" {
  name                 = "${local.name}-backup-schedule"
  assume_role_policy   = data.aws_iam_policy_document.scheduler_assume_role.json
  permissions_boundary = local.role_boundary_arn

  tags = merge(var.tags, {
    Name = "${local.name}-backup-schedule"
  })
}

locals {
  backup_family_arn = "arn:${local.partition}:ecs:${local.region}:${local.account_id}:task-definition/${local.backup_family}"

  # The release's backup revision (its manifest's task_definitions.backup,
  # service_ssm.tf), which `app deploy` and `app rollback` point the schedule
  # at as they record a release: the plan reads the same, so an apply leaves
  # it be. None (before the first release, the schedule off): the family.
  release_backup_task_definition = try(jsondecode(local.release_value).task_definitions.backup, "")
  backup_schedule_task_definition = (
    local.released && startswith(local.release_backup_task_definition, "${local.backup_family_arn}:")
    ? local.release_backup_task_definition : local.backup_family_arn
  )
}

data "aws_iam_policy_document" "backup_schedule" {
  statement {
    sid       = "RunBackupTask"
    effect    = "Allow"
    actions   = ["ecs:RunTask"]
    resources = [local.backup_family_arn, "${local.backup_family_arn}:*"]

    condition {
      test     = "ArnEquals"
      variable = "ecs:cluster"
      values   = [aws_ecs_cluster.main.arn]
    }
  }

  statement {
    sid       = "PassBackupRoles"
    effect    = "Allow"
    actions   = ["iam:PassRole"]
    resources = [aws_iam_role.backup.arn, aws_iam_role.execution.arn]

    condition {
      test     = "StringEquals"
      variable = "iam:PassedToService"
      values   = ["ecs-tasks.amazonaws.com"]
    }
  }

  statement {
    sid       = "TagBackupTask"
    effect    = "Allow"
    actions   = ["ecs:TagResource"]
    resources = ["arn:${local.partition}:ecs:${local.region}:${local.account_id}:task/${aws_ecs_cluster.main.name}/*"]

    condition {
      test     = "StringEquals"
      variable = "ecs:CreateAction"
      values   = ["RunTask"]
    }
  }
}

resource "aws_iam_role_policy" "backup_schedule" {
  name   = "${local.name}-backup-schedule"
  role   = aws_iam_role.backup_schedule.id
  policy = data.aws_iam_policy_document.backup_schedule.json
}

resource "aws_scheduler_schedule" "backup" {
  name        = "${local.name}-backup"
  description = "OpenNova ${local.deployment}: the nightly pg_dump of the database to s3://${local.ops_bucket_name}/${local.backup_prefix}/"

  schedule_expression          = "cron(0 3 * * ? *)"
  schedule_expression_timezone = "UTC"
  # Off until the first release: no program has run, and no backup image is
  # pinned yet.
  state = local.released ? "ENABLED" : "DISABLED"

  flexible_time_window {
    mode = "OFF"
  }

  target {
    arn      = aws_ecs_cluster.main.arn
    role_arn = aws_iam_role.backup_schedule.arn

    ecs_parameters {
      # The release's revision, its image pinned by digest.
      task_definition_arn     = local.backup_schedule_task_definition
      launch_type             = "FARGATE"
      platform_version        = "1.4.0"
      task_count              = 1
      enable_ecs_managed_tags = true
      propagate_tags          = "TASK_DEFINITION"

      network_configuration {
        subnets          = local.program_subnet_ids
        security_groups  = [aws_security_group.backup.id]
        assign_public_ip = true
      }
    }

    retry_policy {
      maximum_event_age_in_seconds = 3600
      maximum_retry_attempts       = 3
    }
  }

  depends_on = [aws_iam_role_policy.backup_schedule, aws_ecs_task_definition.backup]
}
