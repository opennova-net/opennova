# ---------------------------------------------------------------------------
# The fleet's machine agent on ECS: opennova-hostd as a DAEMON service of a
# cluster of its own, one task on every machine (DEPLOY.md "Hosted game
# servers"; apps/hostd/README.md)
# ---------------------------------------------------------------------------
#
# Every machine (fleet.tf: the worker's EC2 launches, no Auto Scaling group,
# no capacity provider) boots the deployment's pinned ECS-optimized Amazon
# Linux 2023 image (var.fleet_ami, by name, so a bump shows in the plan and in
# git; staging first). Its user data writes /etc/ecs/ecs.config: the fleet
# cluster, the instance attributes machine-id and kind, task IAM roles in host
# network mode, no privileged container. The ECS agent registers the machine
# in the cluster, and the hostd service runs one task on it:
#   EC2 launch type, the host's network (the servers bind the machine's own
#   UDP and TCP ports: slot k is 32768+k and 32800+k), user 0, docker's
#   default capabilities and NET_ADMIN (hostd's nft table: the slots' fences
#   and counters), never privileged; the docker socket and the machine's
#   /etc/opennova (the token and machine-id files), /var/lib/opennova-hostd
#   (its state), /srv/slots and /opt/game (the slots' directories and the
#   trees, bound into the servers' containers by the same paths, so each is
#   mounted at its own path) and /etc/ecs (read-only: the attributes hostd
#   checks its machine id against); its task role reads the trees and opens
#   ECS Exec's channels (`fleet shell`); no execution role (the images are
#   public, and the awslogs driver writes hostd's lines, stream
#   hostd/hostd/<task id> of the machines' group, with the instance role);
#   its health check is `opennova-hostd --check-alive` (the agent's loop is
#   turning, not "the last sync worked").
# The servers themselves are no ECS tasks: hostd starts them with `docker run`
# and ECS never sees them. So ecs.config never sets
# ECS_ENABLE_UNTRACKED_IMAGE_CLEANUP (it would delete their images), and the
# fleet cluster is never drained through ECS (a drain stops hostd, not the
# servers; the service drains a machine itself).
#
# The service rolls hostd with each release before the fleet program (`app
# deploy`: a revision of this task definition with the release's image pinned
# by digest; the service ignores its task definition here), at most half the
# machines at once (minimumHealthyPercent 50, the most a DAEMON service takes;
# its maximum percent is ECS's fixed 100). It has no desired count, no Auto
# Scaling target, and no place in local.programs, their precondition or their
# alarms: it runs a task on each machine that registers, and only the
# released worker launches machines.
#
# All of it exists only while var.hosted_enabled is on.

locals {
  fleet_cluster_name = "${local.name}-fleet"
  fleet_cluster_arn  = "arn:${local.partition}:ecs:${local.region}:${local.account_id}:cluster/${local.fleet_cluster_name}"

  # The game servers' log group, by name (observability.tf keeps it): the
  # slots' containers write it with the instance role.
  servers_log_group_arn = "arn:${local.partition}:logs:${local.region}:${local.account_id}:log-group:/opennova/${local.deployment}/servers"

  # The machine's paths hostd works in, each bound at the same path inside
  # its task (the servers' `docker run --mount` names host paths).
  hostd_volumes = {
    docker-socket = { path = "/var/run/docker.sock", read_only = false }
    opennova-etc  = { path = "/etc/opennova", read_only = false }
    hostd-state   = { path = "/var/lib/opennova-hostd", read_only = false }
    slots         = { path = "/srv/slots", read_only = false }
    trees         = { path = "/opt/game", read_only = false }
    ecs-config    = { path = "/etc/ecs", read_only = true }
  }

  hostd_container = {
    name              = "hostd"
    image             = "${var.image_registry}/opennova-hostd:${var.image_tag}"
    essential         = true
    user              = "0"
    cpu               = 128
    memoryReservation = 128
    command = [
      "--service", local.internal_url,
      "--machine-file", "/etc/opennova/machine-id",
      "--ecs-config", "/etc/ecs/ecs.config",
      "--log-group", aws_cloudwatch_log_group.machines.name,
      "--log-region", local.region,
    ]
    mountPoints = [for k in sort(keys(local.hostd_volumes)) : {
      sourceVolume  = k
      containerPath = local.hostd_volumes[k].path
      readOnly      = local.hostd_volumes[k].read_only
    }]
    # Docker's init as PID 1 forwards the stop's signal; NET_ADMIN beside
    # docker's default capabilities, for hostd's nft table.
    linuxParameters = {
      initProcessEnabled = true
      capabilities       = { add = ["NET_ADMIN"] }
    }
    stopTimeout = 30
    healthCheck = {
      command     = ["CMD", "/usr/local/bin/opennova-hostd", "--check-alive"]
      interval    = 30
      timeout     = 5
      retries     = 3
      startPeriod = 60
    }
    logConfiguration = {
      logDriver = "awslogs"
      options = {
        "awslogs-group"         = aws_cloudwatch_log_group.machines.name
        "awslogs-region"        = local.region
        "awslogs-stream-prefix" = "hostd"
        "mode"                  = "non-blocking"
        "max-buffer-size"       = "4m"
      }
    }
  }
}

# The deployment's pinned image: the exact name, Amazon's own. A pinned image
# AWS has since deprecated still launches, so deprecated ones are found too.
data "aws_ami" "fleet" {
  count              = local.hosted_enabled ? 1 : 0
  owners             = ["amazon"]
  most_recent        = true
  include_deprecated = true

  filter {
    name   = "name"
    values = [var.fleet_ami]
  }
  filter {
    name   = "architecture"
    values = ["x86_64"]
  }
  filter {
    name   = "state"
    values = ["available"]
  }

  lifecycle {
    precondition {
      condition     = var.fleet_ami != ""
      error_message = "hosted_enabled needs fleet_ami: the exact name of the ECS-optimized Amazon Linux 2023 x86_64 image the machines launch (aws ssm get-parameters --names /aws/service/ecs/optimized-ami/amazon-linux-2023/recommended/image_name)."
    }
  }
}

# ---- the fleet cluster ---------------------------------------------------------------

resource "aws_ecs_cluster" "fleet" {
  count = local.hosted_enabled ? 1 : 0
  name  = local.fleet_cluster_name

  setting {
    name  = "containerInsights"
    value = "disabled"
  }

  tags = merge(var.tags, {
    Name = local.fleet_cluster_name
  })
}

# ---- hostd's task role ----------------------------------------------------------------

resource "aws_iam_role" "hostd" {
  count                = local.hosted_enabled ? 1 : 0
  name                 = "${local.name}-hostd"
  assume_role_policy   = data.aws_iam_policy_document.ecs_tasks_assume_role.json
  permissions_boundary = local.machine_boundary_arn

  tags = merge(var.tags, {
    Name = "${local.name}-hostd"
  })
}

# The trees (immutable, the service writes them): hostd's `aws s3 sync` lists
# a tree's prefix and reads its objects. The trees alone, read alone: nothing
# of the dumps, the releases or the uploads.
data "aws_iam_policy_document" "hostd" {
  count = local.hosted_enabled ? 1 : 0

  statement {
    sid       = "ListDataTrees"
    effect    = "Allow"
    actions   = ["s3:ListBucket"]
    resources = [local.ops_bucket_arn]

    condition {
      test     = "StringLike"
      variable = "s3:prefix"
      values   = ["${local.data_prefix}/trees/*"]
    }
  }

  statement {
    sid       = "ReadDataTrees"
    effect    = "Allow"
    actions   = ["s3:GetObject"]
    resources = ["${local.ops_bucket_arn}/${local.data_prefix}/trees/*"]
  }
}

resource "aws_iam_role_policy" "hostd" {
  count  = local.hosted_enabled ? 1 : 0
  name   = "${local.name}-hostd"
  role   = aws_iam_role.hostd[0].id
  policy = data.aws_iam_policy_document.hostd[0].json
}

# ECS Exec into a machine's hostd (`fleet shell`), as into every program.
resource "aws_iam_role_policy" "hostd_exec" {
  count  = local.hosted_enabled ? 1 : 0
  name   = "${local.name}-hostd-exec"
  role   = aws_iam_role.hostd[0].id
  policy = data.aws_iam_policy_document.exec.json
}

# ---- hostd's task definition and its DAEMON service ------------------------------------

resource "aws_ecs_task_definition" "hostd" {
  count = local.hosted_enabled ? 1 : 0

  family                   = "${local.name}-hostd"
  requires_compatibilities = ["EC2"]
  network_mode             = "host"
  task_role_arn            = aws_iam_role.hostd[0].arn
  container_definitions    = jsonencode([local.hostd_container])

  dynamic "volume" {
    for_each = local.hostd_volumes
    content {
      name      = volume.key
      host_path = volume.value.path
    }
  }

  runtime_platform {
    operating_system_family = "LINUX"
    cpu_architecture        = "X86_64"
  }

  tags = merge(var.tags, {
    Name = "${local.name}-hostd"
  })
}

resource "aws_ecs_service" "hostd" {
  count = local.hosted_enabled ? 1 : 0

  name                = "hostd"
  cluster             = aws_ecs_cluster.fleet[0].id
  task_definition     = aws_ecs_task_definition.hostd[0].arn
  launch_type         = "EC2"
  scheduling_strategy = "DAEMON"
  # A bad revision reaches half the machines at most before the deploy
  # points hostd back.
  deployment_minimum_healthy_percent = 50
  enable_execute_command             = true
  enable_ecs_managed_tags            = true
  propagate_tags                     = "SERVICE"

  # Terraform does not wait: no task runs until a machine registers.
  wait_for_steady_state = false

  tags = merge(var.tags, {
    Name = "${local.name}-hostd"
  })

  lifecycle {
    # The deploy's revisions.
    ignore_changes = [task_definition]
  }
}

# ---- web's reads of the fleet cluster --------------------------------------------------

# The admin's infrastructure view (apps/novaworld_web/infra/): the fleet
# cluster's container instances (each machine's agent, its attributes, its
# image) and the hostd service, read-only, the fleet cluster's alone
# (ONNET_INFRA_FLEET_CLUSTER). The role boundary holds the same.
data "aws_iam_policy_document" "web_fleet_read" {
  count = local.hosted_enabled ? 1 : 0

  statement {
    sid       = "ListFleetInstances"
    effect    = "Allow"
    actions   = ["ecs:ListContainerInstances"]
    resources = [local.fleet_cluster_arn]
  }

  statement {
    sid       = "DescribeFleetInstances"
    effect    = "Allow"
    actions   = ["ecs:DescribeContainerInstances"]
    resources = ["arn:${local.partition}:ecs:${local.region}:${local.account_id}:container-instance/${local.fleet_cluster_name}/*"]
  }

  statement {
    sid       = "ReadFleetServices"
    effect    = "Allow"
    actions   = ["ecs:DescribeServices"]
    resources = ["arn:${local.partition}:ecs:${local.region}:${local.account_id}:service/${local.fleet_cluster_name}/*"]
  }
}

resource "aws_iam_role_policy" "web_fleet_read" {
  count  = local.hosted_enabled ? 1 : 0
  name   = "${local.name}-web-fleet-read"
  role   = aws_iam_role.program["web"].id
  policy = data.aws_iam_policy_document.web_fleet_read[0].json
}
