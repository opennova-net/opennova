# ---------------------------------------------------------------------------
# The cluster the programs run in, the controller's private name, and the
# role ECS starts every task with (DEPLOY.md, "Operating")
# ---------------------------------------------------------------------------
#
# One ECS cluster per deployment, Fargate alone (no instances to keep). The
# controller's tasks register in a Cloud Map private DNS namespace,
# <deployment>.opennova.internal (a Route 53 zone private to the VPC that Cloud
# Map keeps), as `controller`: an A record of each running task's private
# address, TTL 10, which the machines' hostd and ONNET_INTERNAL_URL name. The
# task execution role pulls nothing (the images are public on GHCR) and does
# two things for ECS before a container starts: it reads the task's secrets
# from SSM (/opennova/<deployment>/secret/* and tls/*) and it ships the
# containers' logs.

resource "aws_ecs_cluster" "main" {
  name = local.name

  # ECS's own service metrics (CPU, memory) are enough: Container Insights
  # bills each of its metrics.
  setting {
    name  = "containerInsights"
    value = "disabled"
  }

  tags = merge(var.tags, {
    Name = local.name
  })
}

# ---- the controller's name -------------------------------------------------------

locals {
  internal_zone_name = "${local.deployment}.opennova.internal"
  controller_host    = "controller.${local.internal_zone_name}"
  internal_url       = "http://${local.controller_host}:${var.internal_http_port}"
}

resource "aws_service_discovery_private_dns_namespace" "internal" {
  name        = local.internal_zone_name
  description = "OpenNova ${local.deployment}: the controller's private name"
  vpc         = aws_vpc.main.id

  tags = var.tags
}

# Each running controller task's private address, by name. The leader alone
# binds the internal listener; a standby beside it (a deployment's new task)
# is listed too for its short overlap, and hostd tries each address the name
# lists until one takes its connection (apps/hostd/agent.h, HttpServiceLink).
resource "aws_service_discovery_service" "controller" {
  name = "controller"

  dns_config {
    namespace_id   = aws_service_discovery_private_dns_namespace.internal.id
    routing_policy = "MULTIVALUE"

    dns_records {
      ttl  = 10
      type = "A"
    }
  }

  # ECS reports each task's health (its container health check).
  health_check_custom_config {
    failure_threshold = 1
  }

  tags = var.tags
}

# ---- the task execution role ---------------------------------------------------------

resource "aws_iam_role" "execution" {
  name                 = "${local.name}-execution"
  assume_role_policy   = data.aws_iam_policy_document.ecs_tasks_assume_role.json
  permissions_boundary = local.role_boundary_arn

  tags = merge(var.tags, {
    Name = "${local.name}-execution"
  })
}

# The deployment's secrets and the origin certificate alone (SecureStrings
# under the AWS-managed aws/ssm key, which needs no KMS grant here), and the
# service's log group.
data "aws_iam_policy_document" "execution" {
  statement {
    sid       = "ReadTaskSecrets"
    effect    = "Allow"
    actions   = ["ssm:GetParameters"]
    resources = ["${local.ssm_parameter_prefix}/secret/*", "${local.ssm_parameter_prefix}/tls/*"]
  }

  statement {
    sid       = "ShipLogs"
    effect    = "Allow"
    actions   = ["logs:CreateLogStream", "logs:PutLogEvents"]
    resources = [aws_cloudwatch_log_group.service.arn, "${aws_cloudwatch_log_group.service.arn}:*"]
  }
}

resource "aws_iam_role_policy" "execution" {
  name   = "${local.name}-execution"
  role   = aws_iam_role.execution.id
  policy = data.aws_iam_policy_document.execution.json
}
