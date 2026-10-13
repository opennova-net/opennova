# ---------------------------------------------------------------------------
# Hosted game servers (ADR 0053; DEPLOY.md "Hosted game servers")
# ---------------------------------------------------------------------------
#
# One EC2 machine per hosted server. The NovaWorld service launches and ends
# them itself (apps/novaworld_server/hosted/ec2_provider.h) through the AWS
# CLI, with the service instance's role. This file gives it what it launches
# from and the least it may do:
#   - the launch template: a stock Ubuntu 24.04 image, var.hosted_instance_type,
#     an encrypted root volume, the game data volume made from the private
#     encrypted snapshot (var.hosted_data_snapshot_id; the machine mounts it
#     read-only), the game security group, the machine role, IMDSv2 required,
#     and InstanceInitiatedShutdownBehavior=terminate (a machine powering off
#     is a machine ended: opennova-serve's end powers it off);
#   - the game security group: the game port to players, the admin port to the
#     service's security group alone, and the service's security group
#     admitting the machines on its HTTP port (their bootstrap fetch, over the
#     private network);
#   - the machine role: the five actions SSM Session Manager needs for
#     break-glass, nothing else (no machine has an SSH key);
#   - the service role's policy (the one instance profile the service instance
#     carries, aws_iam_role.backup): launch only from this template, only the
#     template's machine type with IMDSv2 required, gp3 volumes of at most
#     hosted_max_volume_gb and Canonical's images, with this deployment's tags;
#     tag only at launch, end only machines carrying both tags, describe, and
#     pass the machine role.
#
# All of it exists only while var.hosted_data_snapshot_id names the snapshot.
# Empty (the default), this file creates nothing and changes nothing: the plan
# shows its outputs alone, and aws_instance.server and its security group stay
# as they are.
#
# The ports are pinned with the service's (ec2_provider.h kGamePort and
# kAdminPort; game.cfg pins mpnovaworldportmin = max to the game port): change
# them in both places or in neither.

locals {
  hosted_enabled    = var.hosted_data_snapshot_id != ""
  hosted_deployment = var.hosted_deployment != "" ? var.hosted_deployment : local.name
  hosted_game_port  = 32768
  hosted_admin_port = 32800
  # The only tags a launch may carry (the service's run-instances names these).
  hosted_tag_keys = ["opennova:hosted", "opennova:deployment", "opennova:server-id", "Name"]
  # The largest volume a launch may make: the root volume, and the data volume
  # (the snapshot's size, 2 GiB for a JO:CA install).
  hosted_max_volume_gb = max(16, var.hosted_root_volume_gb)
  # Canonical, the stock Ubuntu images' owner; var.hosted_ami_id's own owner
  # when one is named.
  hosted_image_owner = "099720109477"
}

data "aws_caller_identity" "hosted" {
  count = local.hosted_enabled ? 1 : 0
}

data "aws_partition" "hosted" {
  count = local.hosted_enabled ? 1 : 0
}

# The machines' image: Canonical's newest Ubuntu 24.04 when the launch template
# is made, or var.hosted_ami_id. The template ignores later image changes, as
# aws_instance.server does (#1043): a new image is a deliberate
# `terraform apply -replace='aws_launch_template.hosted[0]'`, then an
# `app deploy` (the service reads the template's id from the outputs). Running
# machines never move: each keeps the image it launched with until it ends.
data "aws_ami" "hosted" {
  count       = local.hosted_enabled ? 1 : 0
  most_recent = true
  owners      = var.hosted_ami_id != "" ? null : ["099720109477"]

  dynamic "filter" {
    for_each = var.hosted_ami_id != "" ? tomap({
      "image-id" = [var.hosted_ami_id]
      }) : tomap({
      "name" = [
        "ubuntu/images/hvm-ssd/ubuntu-noble-24.04-amd64-server-*",
        "ubuntu/images/*ubuntu-noble-24.04-amd64-server-*",
      ]
      "virtualization-type" = ["hvm"]
      "architecture"        = ["x86_64"]
      "root-device-type"    = ["ebs"]
      "state"               = ["available"]
    })
    content {
      name   = filter.key
      values = filter.value
    }
  }
}

# The subnet the machines launch into (the service's own): its range is the
# one the service's bootstrap route answers (ONNET_HOSTED_BOOTSTRAP_CIDRS).
data "aws_subnet" "hosted" {
  count = local.hosted_enabled ? 1 : 0
  id    = local.target_subnet
}

# A machine needs a public address of its own (players join it there, and it
# lists through the service's public one): the subnet must hand one out.
check "hosted_subnet_assigns_public_ips" {
  assert {
    condition     = !local.hosted_enabled || one(data.aws_subnet.hosted[*].map_public_ip_on_launch) == true
    error_message = "Hosted game servers launch into the service's subnet, which must auto-assign public IPv4 addresses (map_public_ip_on_launch); the auto-created subnet does."
  }
}

# ---- network ----------------------------------------------------------------

resource "aws_security_group" "hosted" {
  count       = local.hosted_enabled ? 1 : 0
  name        = "${local.name}-hosted-sg"
  description = "OpenNova hosted game server machines: the game port to players, remote admin to the NovaWorld service alone"
  vpc_id      = local.target_vpc_id

  tags = merge(var.tags, {
    Name = "${local.name}-hosted"
  })
}

# Rules as resources of their own (aws_security_group.server admits this group
# inline, so an inline rule here referring back would make a cycle).
resource "aws_vpc_security_group_ingress_rule" "hosted_game" {
  for_each = local.hosted_enabled ? toset(var.allowed_cidrs) : toset([])

  security_group_id = aws_security_group.hosted[0].id
  description       = "Hosted server game UDP (the NovaWorld window, pinned to one port)"
  ip_protocol       = "udp"
  from_port         = local.hosted_game_port
  to_port           = local.hosted_game_port
  cidr_ipv4         = each.value
}

resource "aws_vpc_security_group_ingress_rule" "hosted_admin" {
  count = local.hosted_enabled ? 1 : 0

  security_group_id            = aws_security_group.hosted[0].id
  description                  = "Remote admin from the NovaWorld service alone (its admin link, over the private network)"
  ip_protocol                  = "tcp"
  from_port                    = local.hosted_admin_port
  to_port                      = local.hosted_admin_port
  referenced_security_group_id = aws_security_group.server.id
}

# Out: the image pull (ghcr.io), Ubuntu's mirrors, the bootstrap fetch, and
# NovaWorld at the service's public address.
resource "aws_vpc_security_group_egress_rule" "hosted_all" {
  count = local.hosted_enabled ? 1 : 0

  security_group_id = aws_security_group.hosted[0].id
  description       = "All outbound"
  ip_protocol       = "-1"
  cidr_ipv4         = "0.0.0.0/0"
}

# ---- the machine role ---------------------------------------------------------

resource "aws_iam_role" "hosted_machine" {
  count              = local.hosted_enabled ? 1 : 0
  name               = "${local.name}-hosted-machine-role"
  assume_role_policy = data.aws_iam_policy_document.backup_assume_role.json

  tags = merge(var.tags, {
    Name = "${local.name}-hosted-machine-role"
  })
}

# Session Manager for break-glass (aws ssm start-session): the agent's
# registration and its session channels, and nothing else. Not the managed
# AmazonSSMManagedInstanceCore, which also reads every SSM parameter and runs
# the agent's other channels. (The machine also fences the server's UID off
# the metadata service, so the server never holds even these.)
data "aws_iam_policy_document" "hosted_machine_ssm" {
  count = local.hosted_enabled ? 1 : 0

  statement {
    sid    = "SessionManagerOnly"
    effect = "Allow"
    actions = [
      "ssm:UpdateInstanceInformation",
      "ssmmessages:CreateControlChannel",
      "ssmmessages:CreateDataChannel",
      "ssmmessages:OpenControlChannel",
      "ssmmessages:OpenDataChannel",
    ]
    resources = ["*"]
  }
}

resource "aws_iam_role_policy" "hosted_machine_ssm" {
  count  = local.hosted_enabled ? 1 : 0
  name   = "${local.name}-hosted-machine-ssm"
  role   = aws_iam_role.hosted_machine[0].id
  policy = data.aws_iam_policy_document.hosted_machine_ssm[0].json
}

resource "aws_iam_instance_profile" "hosted_machine" {
  count = local.hosted_enabled ? 1 : 0
  name  = "${local.name}-hosted-machine-profile"
  role  = aws_iam_role.hosted_machine[0].name
}

# ---- the launch template --------------------------------------------------------

resource "aws_launch_template" "hosted" {
  count       = local.hosted_enabled ? 1 : 0
  name        = "${local.name}-hosted"
  description = "OpenNova hosted game server machines (ADR 0053): one per hosted server, launched by the NovaWorld service"

  image_id                             = data.aws_ami.hosted[0].id
  instance_type                        = var.hosted_instance_type
  instance_initiated_shutdown_behavior = "terminate"
  vpc_security_group_ids               = [aws_security_group.hosted[0].id]
  update_default_version               = true

  iam_instance_profile {
    arn = aws_iam_instance_profile.hosted_machine[0].arn
  }

  metadata_options {
    http_endpoint               = "enabled"
    http_tokens                 = "required"
    http_put_response_hop_limit = 1
    instance_metadata_tags      = "disabled"
  }

  # The root volume: the OS, docker and the server's working directory (its
  # secret files among them).
  block_device_mappings {
    device_name = data.aws_ami.hosted[0].root_device_name

    ebs {
      volume_size           = var.hosted_root_volume_gb
      volume_type           = "gp3"
      encrypted             = true
      delete_on_termination = true
    }
  }

  # The game data: the private snapshot's volume, its size the snapshot's,
  # labelled opennova-jo, mounted read-only at /opt/jo.
  block_device_mappings {
    device_name = "/dev/sdf"

    ebs {
      snapshot_id           = var.hosted_data_snapshot_id
      volume_type           = "gp3"
      encrypted             = true
      delete_on_termination = true
    }
  }

  tags = merge(var.tags, {
    Name = "${local.name}-hosted"
  })

  lifecycle {
    ignore_changes = [image_id]
  }
}

# ---- what the service may do ----------------------------------------------------

locals {
  hosted_arn_prefix = local.hosted_enabled ? "arn:${data.aws_partition.hosted[0].partition}:ec2:${var.region}" : ""
  hosted_account    = local.hosted_enabled ? data.aws_caller_identity.hosted[0].account_id : ""
}

data "aws_iam_policy_document" "hosted_service" {
  count = local.hosted_enabled ? 1 : 0

  # A machine launches only from this template, as the template's machine type
  # with IMDSv2 required (a caller's own --instance-type or metadata options
  # are refused), with this deployment's tags and no other tag.
  statement {
    sid       = "LaunchHostedMachines"
    effect    = "Allow"
    actions   = ["ec2:RunInstances"]
    resources = ["${local.hosted_arn_prefix}:${local.hosted_account}:instance/*"]

    condition {
      test     = "ArnEquals"
      variable = "ec2:LaunchTemplate"
      values   = [aws_launch_template.hosted[0].arn]
    }
    condition {
      test     = "StringEquals"
      variable = "ec2:InstanceType"
      values   = [var.hosted_instance_type]
    }
    condition {
      test     = "StringEquals"
      variable = "ec2:MetadataHttpTokens"
      values   = ["required"]
    }
    condition {
      test     = "StringEquals"
      variable = "aws:RequestTag/opennova:hosted"
      values   = ["1"]
    }
    condition {
      test     = "StringEquals"
      variable = "aws:RequestTag/opennova:deployment"
      values   = [local.hosted_deployment]
    }
    condition {
      test     = "ForAllValues:StringEquals"
      variable = "aws:TagKeys"
      values   = local.hosted_tag_keys
    }
  }

  # What such a launch touches besides the machine: its volumes (gp3, no
  # larger than the template's), its network interface, the template's own
  # subnet, security group and snapshot, and a Canonical image. RunInstances
  # on these creates nothing without the machine statement above. The snapshot
  # is encrypted with the AWS-managed aws/ebs key, whose key policy admits
  # every principal of the account through EBS: no KMS grant here (a
  # customer-managed key would need kms:CreateGrant, kms:Decrypt and
  # kms:GenerateDataKeyWithoutPlaintext on it).
  statement {
    sid       = "LaunchHostedMachineVolumes"
    effect    = "Allow"
    actions   = ["ec2:RunInstances"]
    resources = ["${local.hosted_arn_prefix}:${local.hosted_account}:volume/*"]

    condition {
      test     = "StringEquals"
      variable = "ec2:VolumeType"
      values   = ["gp3"]
    }
    condition {
      test     = "NumericLessThanEquals"
      variable = "ec2:VolumeSize"
      values   = [tostring(local.hosted_max_volume_gb)]
    }
  }

  statement {
    sid       = "LaunchHostedMachineImage"
    effect    = "Allow"
    actions   = ["ec2:RunInstances"]
    resources = ["${local.hosted_arn_prefix}::image/*"]

    condition {
      test     = "StringEquals"
      variable = "ec2:Owner"
      values   = [var.hosted_ami_id != "" ? data.aws_ami.hosted[0].owner_id : local.hosted_image_owner]
    }
  }

  statement {
    sid     = "LaunchHostedMachineResources"
    effect  = "Allow"
    actions = ["ec2:RunInstances"]
    resources = [
      "${local.hosted_arn_prefix}:${local.hosted_account}:network-interface/*",
      "${local.hosted_arn_prefix}:${local.hosted_account}:subnet/${local.target_subnet}",
      "${local.hosted_arn_prefix}:${local.hosted_account}:security-group/${aws_security_group.hosted[0].id}",
      "${local.hosted_arn_prefix}:${local.hosted_account}:launch-template/${aws_launch_template.hosted[0].id}",
      "${local.hosted_arn_prefix}::snapshot/${var.hosted_data_snapshot_id}",
    ]
  }

  # Tags only as part of a launch.
  statement {
    sid     = "TagHostedMachinesAtLaunch"
    effect  = "Allow"
    actions = ["ec2:CreateTags"]
    resources = [
      "${local.hosted_arn_prefix}:${local.hosted_account}:instance/*",
      "${local.hosted_arn_prefix}:${local.hosted_account}:volume/*",
    ]

    condition {
      test     = "StringEquals"
      variable = "ec2:CreateAction"
      values   = ["RunInstances"]
    }
  }

  # Ends only this deployment's hosted machines.
  statement {
    sid       = "EndHostedMachines"
    effect    = "Allow"
    actions   = ["ec2:TerminateInstances"]
    resources = ["${local.hosted_arn_prefix}:${local.hosted_account}:instance/*"]

    condition {
      test     = "StringEquals"
      variable = "aws:ResourceTag/opennova:hosted"
      values   = ["1"]
    }
    condition {
      test     = "StringEquals"
      variable = "aws:ResourceTag/opennova:deployment"
      values   = [local.hosted_deployment]
    }
  }

  # Describe calls take no resource scope.
  statement {
    sid       = "DescribeHostedMachines"
    effect    = "Allow"
    actions   = ["ec2:DescribeInstances", "ec2:DescribeLaunchTemplates", "ec2:DescribeLaunchTemplateVersions"]
    resources = ["*"]
  }

  # The machine role, to EC2 alone.
  statement {
    sid       = "PassHostedMachineRole"
    effect    = "Allow"
    actions   = ["iam:PassRole"]
    resources = [aws_iam_role.hosted_machine[0].arn]

    condition {
      test     = "StringEquals"
      variable = "iam:PassedToService"
      values   = ["ec2.amazonaws.com"]
    }
  }
}

resource "aws_iam_policy" "hosted_service" {
  count       = local.hosted_enabled ? 1 : 0
  name        = "${local.name}-hosted-service-policy"
  description = "The NovaWorld service's hosted game server machines: launch from the template, end its own"
  policy      = data.aws_iam_policy_document.hosted_service[0].json
}

# The service instance carries one instance profile (aws_iam_instance_profile.
# backup): its role takes this policy beside the backups'. An in-place change,
# never a new instance.
resource "aws_iam_role_policy_attachment" "hosted_service" {
  count      = local.hosted_enabled ? 1 : 0
  role       = aws_iam_role.backup.name
  policy_arn = aws_iam_policy.hosted_service[0].arn
}
