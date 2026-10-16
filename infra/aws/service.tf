# ---------------------------------------------------------------------------
# The service: what its five programs share (programs.tf makes the programs;
# DEPLOY.md, "Operating")
# ---------------------------------------------------------------------------
#
# Five programs, each its own image and an ECS service on Fargate: gate (UDP,
# behind load_balancer.tf's network load balancer), lobby (UDP on each task's
# own public address), web (the HTTP routes and the website's API, with the
# portal's nginx, behind the same balancer), controller (the fleet's reconcile
# pass, the jobs' scheduling, the machines' internal listener) and worker (the
# jobs). No task keeps state of its own: secrets live in SSM Parameter Store
# (the tasks read them at their start), the release in SSM and the operations
# bucket, logs in CloudWatch, everything durable in the database (database.tf),
# web's ephemeral state in the cache (cache.tf).
#
# Here: the account's names, the roles' common trust, and the gate's Elastic
# IP. Retail clients find the gate by it (gs.novaworld.net, the Switcher's
# hosts block), so prod adopts its existing address by allocation id
# (var.service_eip_allocation_id: the address transferred into the prod
# account): Terraform only reads it, so no plan can release it. Prod never
# makes an address, and staging never adopts one: it gets an address of its
# own, managed here. A plan that breaks either rule stops at its precondition. The address is the load balancer's in
# zone a (its subnet mapping, load_balancer.tf), where the gate's UDP port and
# web's HTTP routes listen: ONNET_PUBLIC_HOST and ONNET_WEB_HOST name it.
# No SSH anywhere: a shell is ECS Exec (`app shell`).

data "aws_caller_identity" "current" {}

data "aws_partition" "current" {}

locals {
  account_id = data.aws_caller_identity.current.account_id
  # Every role the stack makes carries this boundary (infra/aws/iam/opennova-role-boundary.json,
  # which infra/org makes in the deployment's account): the deploy user may create and change
  # opennova-* roles only under it, so no role it writes can hold more than OpenNova's own resources.
  role_boundary_arn = "arn:${data.aws_partition.current.partition}:iam::${data.aws_caller_identity.current.account_id}:policy/opennova-role-boundary"
  partition         = data.aws_partition.current.partition

  adopt_eip                 = var.service_eip_allocation_id != ""
  service_eip_allocation_id = local.adopt_eip ? data.aws_eip.gate[0].id : aws_eip.gate[0].allocation_id
  service_public_ip         = local.adopt_eip ? data.aws_eip.gate[0].public_ip : aws_eip.gate[0].public_ip

  ops_bucket_arn       = "arn:${local.partition}:s3:::${local.ops_bucket_name}"
  ssm_parameter_prefix = "arn:${local.partition}:ssm:${local.region}:${local.account_id}:parameter${local.ssm_prefix}"
}

# ---- the gate's address ------------------------------------------------------

# Prod's Elastic IP, adopted: read by its allocation id, never managed. Prod
# alone adopts one: staging naming an allocation id (a stray *.auto.tfvars or
# TF_VAR_ holding prod's, say) has no address of its own to adopt, so its plan
# stops here, before the address is read.
data "aws_eip" "gate" {
  count = local.adopt_eip ? 1 : 0
  id    = var.service_eip_allocation_id

  lifecycle {
    precondition {
      condition     = local.is_prod
      error_message = "deployment ${local.deployment} names an Elastic IP to adopt (service_eip_allocation_id = ${var.service_eip_allocation_id}): prod alone adopts one. Leave service_eip_allocation_id unset off prod (infra/aws/${local.deployment}.tfvars, TF_VAR_service_eip_allocation_id); ${local.deployment} gets an address of its own."
    }
  }
}

# The adopted address carries the deployment's tag like everything the stack
# makes (the provider's default tags reach only resources Terraform manages).
resource "aws_ec2_tag" "gate_eip" {
  count       = local.adopt_eip ? 1 : 0
  resource_id = data.aws_eip.gate[0].id
  key         = "opennova:deployment"
  value       = local.hosted_deployment
}

# Staging gets one of its own. Prod never makes one: its address is the one
# retail clients use, so a prod plan that names none stops here rather than
# give prod a new address.
resource "aws_eip" "gate" {
  count  = local.adopt_eip ? 0 : 1
  domain = "vpc"

  tags = merge(var.tags, {
    Name = "${local.name}-eip"
  })

  lifecycle {
    precondition {
      condition     = !local.is_prod
      error_message = "prod names no Elastic IP to adopt: put the allocation id of the address retail clients use, in the prod account (./deploy/run.sh --deployment prod aws ec2 describe-addresses), in prod's vault item, which the toolbox hands Terraform as TF_VAR_service_eip_allocation_id. Prod never makes an address of its own; it adopts the one transferred into its account (DEPLOY.md, \"The var-file\")."
    }
  }
}

# ---- who may assume the stack's roles -------------------------------------------

# The tasks' roles (programs.tf, backup.tf): ECS on this account's behalf alone.
data "aws_iam_policy_document" "ecs_tasks_assume_role" {
  statement {
    effect  = "Allow"
    actions = ["sts:AssumeRole"]

    principals {
      type        = "Service"
      identifiers = ["ecs-tasks.amazonaws.com"]
    }

    condition {
      test     = "StringEquals"
      variable = "aws:SourceAccount"
      values   = [local.account_id]
    }
  }
}

# The machines' role (fleet.tf).
data "aws_iam_policy_document" "ec2_assume_role" {
  statement {
    effect = "Allow"

    principals {
      type        = "Service"
      identifiers = ["ec2.amazonaws.com"]
    }

    actions = ["sts:AssumeRole"]
  }
}
