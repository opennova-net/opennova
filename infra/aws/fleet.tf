# ---------------------------------------------------------------------------
# The fleet: the machines hosted game servers run on (ADR 0053 as amended for
# machines and slots; DEPLOY.md "Hosted game servers")
# ---------------------------------------------------------------------------
#
# The NovaWorld service launches and ends the machines itself (its EC2 machine
# driver, through the AWS CLI, with the service instance's role); each runs
# opennova-hostd and one opennova-serve container per slot. Two kinds:
#   shared  var.shared_instance_type (t3a.large, unlimited CPU credits), up to
#           var.shared_slots servers of any accounts, packed best-fit;
#   own     var.own_instance_type (c7a.medium), exactly one server, the paid
#           own-machine add-on.
# Slot k of a machine: game UDP 32768+k (to players), admin TCP 32800+k (to the
# service instance's security group alone). Every machine mounts every data set's
# snapshot (var.hosted_data_snapshots) as a volume of its own, read-only.
#
# This file gives the service what it launches from and the least it may do:
#   - a launch template per kind: a stock Ubuntu 24.04 image, the kind's type
#     (a burstable one with unlimited credits), an encrypted root volume, one
#     encrypted volume per data set from its private snapshot, the machines'
#     security group, the machine role, IMDSv2 with a hop limit of 1, and
#     InstanceInitiatedShutdownBehavior=terminate (a machine powering off is a
#     machine ended). The service names the template by kind
#     (ONNET_HOSTED_EC2_LAUNCH_TEMPLATES in infra.env) and hands each launch its
#     user data;
#   - the machines' security group: the slots' game ports to players, their
#     admin ports to the service instance alone; and the service instance admitting
#     the machines on its internal port (var.internal_http_port, its second
#     listener on its fixed private address: hostd's bootstrap and sync);
#   - the machine role: the five actions Session Manager needs for break-glass
#     and its logs (/opennova/<deployment>/machines), nothing else (no machine
#     has an SSH key or database credentials);
#   - the service role's policy: launch only from these templates, only their
#     two types with IMDSv2 required, gp3 volumes no larger than the largest
#     the templates make, the templates' own images and snapshots, this
#     deployment's tags and no other tag; tag only at launch, end only
#     machines carrying both of this deployment's tags, describe, and pass the
#     machine role to EC2.
#
# All of it exists only while var.hosted_data_snapshots names a snapshot; empty
# (the default), hosting is off and infra.env says so.
#
# The ports are the service's and hostd's (apps/novaworld_server/hosted/slots.h):
# change them in both places or in neither.

locals {
  hosted_enabled    = length(var.hosted_data_snapshots) > 0
  hosted_deployment = var.hosted_deployment != "" ? var.hosted_deployment : local.name

  slot_game_port  = 32768
  slot_admin_port = 32800

  machine_kinds = {
    shared = { instance_type = var.shared_instance_type, slots = var.shared_slots }
    own    = { instance_type = var.own_instance_type, slots = 1 }
  }

  # The only tags a launch may carry (the service's run-instances names these).
  fleet_tag_keys = ["opennova:hosted", "opennova:deployment", "opennova:machine-kind", "opennova:machine-id", "opennova:server-id", "Name"]

  # Each data set's volume: a device of its own, in the data sets' sorted
  # order (/dev/sdf, /dev/sdg, ...). The machine finds each by its file
  # system's label, not by the device.
  data_set_devices = { for i, slug in sort(keys(var.hosted_data_snapshots)) : slug => "/dev/sd${substr("fghijklm", i, 1)}" }

  # The largest volume a launch may make: the root volume, and the largest data
  # volume (each snapshot's size, 2 GiB for a JO:CA install).
  fleet_max_volume_gb = max(16, var.machine_root_volume_gb, [for s in data.aws_ebs_snapshot.hosted : s.volume_size]...)
}

# The machines' image: Canonical's newest Ubuntu 24.04 when a launch template
# is made, or var.hosted_ami_id. The templates ignore later image changes, as
# the service instance does: a new image is a deliberate
# `infra apply -replace='aws_launch_template.fleet["shared"]'
# -replace='aws_launch_template.fleet["own"]'`. Running machines keep the image
# they launched with until they end.
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

# Each data set's snapshot, read so its size is explicit in the templates (and
# so a mistyped or deleted id fails at plan, not at a launch).
data "aws_ebs_snapshot" "hosted" {
  for_each     = var.hosted_data_snapshots
  owners       = ["self"]
  snapshot_ids = [each.value]
}

# ---- network ----------------------------------------------------------------

resource "aws_security_group" "machines" {
  count       = local.hosted_enabled ? 1 : 0
  name        = "${local.name}-machines"
  description = "OpenNova hosted game server machines: the slots' game ports to players, their admin ports to the NovaWorld service alone"
  vpc_id      = aws_vpc.main.id

  tags = merge(var.tags, {
    Name = "${local.name}-machines"
  })
}

# Rules as resources of their own (the service's group admits this group and
# this group the service's, so inline rules would make a cycle).
resource "aws_vpc_security_group_ingress_rule" "machines_game" {
  for_each = local.hosted_enabled ? toset(var.allowed_cidrs) : toset([])

  security_group_id = aws_security_group.machines[0].id
  description       = "The slots' game UDP (slot k: ${local.slot_game_port}+k)"
  ip_protocol       = "udp"
  from_port         = local.slot_game_port
  to_port           = local.slot_game_port + var.shared_slots - 1
  cidr_ipv4         = each.value
}

resource "aws_vpc_security_group_ingress_rule" "machines_admin" {
  count = local.hosted_enabled ? 1 : 0

  security_group_id            = aws_security_group.machines[0].id
  description                  = "The slots' remote admin from the NovaWorld service alone (slot k: ${local.slot_admin_port}+k)"
  ip_protocol                  = "tcp"
  from_port                    = local.slot_admin_port
  to_port                      = local.slot_admin_port + var.shared_slots - 1
  referenced_security_group_id = aws_security_group.service.id
}

# Out: the image pulls (ghcr.io), Ubuntu's mirrors, the service's internal port,
# and NovaWorld at the service's public address.
resource "aws_vpc_security_group_egress_rule" "machines_all" {
  count = local.hosted_enabled ? 1 : 0

  security_group_id = aws_security_group.machines[0].id
  description       = "All outbound"
  ip_protocol       = "-1"
  cidr_ipv4         = "0.0.0.0/0"
}

# The service instance admits the machines on its internal port alone: its second
# listener, on its fixed private address (ONNET_INTERNAL_HTTP_BIND), where
# hostd fetches its bootstrap and syncs.
resource "aws_vpc_security_group_ingress_rule" "service_from_machines" {
  count = local.hosted_enabled ? 1 : 0

  security_group_id            = aws_security_group.service.id
  description                  = "The machines' hostd: bootstrap and sync on the service's internal listener (private)"
  ip_protocol                  = "tcp"
  from_port                    = var.internal_http_port
  to_port                      = var.internal_http_port
  referenced_security_group_id = aws_security_group.machines[0].id
}

# ---- the machine role ---------------------------------------------------------

resource "aws_iam_role" "machine" {
  count              = local.hosted_enabled ? 1 : 0
  name               = "${local.name}-machine"
  assume_role_policy = data.aws_iam_policy_document.ec2_assume_role.json

  tags = merge(var.tags, {
    Name = "${local.name}-machine"
  })
}

# Session Manager for break-glass (aws ssm start-session): the agent's
# registration and its session channels; and the machine's logs. Not the managed
# AmazonSSMManagedInstanceCore, which also reads every SSM parameter in the
# account. (The machine also fences the slots' UIDs off the metadata service,
# so no server holds even these.)
data "aws_iam_policy_document" "machine" {
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

  statement {
    sid       = "ShipMachineLogs"
    effect    = "Allow"
    actions   = ["logs:CreateLogStream", "logs:PutLogEvents", "logs:DescribeLogStreams"]
    resources = [aws_cloudwatch_log_group.machines.arn, "${aws_cloudwatch_log_group.machines.arn}:*"]
  }
}

resource "aws_iam_role_policy" "machine" {
  count  = local.hosted_enabled ? 1 : 0
  name   = "${local.name}-machine"
  role   = aws_iam_role.machine[0].id
  policy = data.aws_iam_policy_document.machine[0].json
}

resource "aws_iam_instance_profile" "machine" {
  count = local.hosted_enabled ? 1 : 0
  name  = "${local.name}-machine"
  role  = aws_iam_role.machine[0].name
}

# ---- the launch templates, one per kind ---------------------------------------

resource "aws_launch_template" "fleet" {
  for_each    = local.hosted_enabled ? local.machine_kinds : {}
  name        = "${local.name}-${each.key}-machine"
  description = "OpenNova ${each.key} machines (${each.value.slots} slot${each.value.slots == 1 ? "" : "s"}), launched by the NovaWorld service"

  image_id                             = data.aws_ami.hosted[0].id
  instance_type                        = each.value.instance_type
  instance_initiated_shutdown_behavior = "terminate"
  vpc_security_group_ids               = [aws_security_group.machines[0].id]
  update_default_version               = true

  iam_instance_profile {
    arn = aws_iam_instance_profile.machine[0].arn
  }

  metadata_options {
    http_endpoint               = "enabled"
    http_tokens                 = "required"
    http_put_response_hop_limit = 1
    instance_metadata_tags      = "disabled"
  }

  # A burstable type runs here with unlimited credits alone: spent credits
  # would throttle every slot's 62.5 Hz tick (the variables' validation
  # refuses the types that cannot run with them).
  dynamic "credit_specification" {
    for_each = startswith(each.value.instance_type, "t") ? [1] : []
    content {
      cpu_credits = "unlimited"
    }
  }

  # The root volume: the OS, docker, the serve image and the slots' working
  # directories (their secret files among them).
  block_device_mappings {
    device_name = data.aws_ami.hosted[0].root_device_name

    ebs {
      volume_size           = var.machine_root_volume_gb
      volume_type           = "gp3"
      encrypted             = true
      delete_on_termination = true
    }
  }

  # Every data set: its private snapshot's volume, its size the snapshot's,
  # mounted read-only by its file system's label.
  dynamic "block_device_mappings" {
    for_each = local.data_set_devices
    content {
      device_name = block_device_mappings.value

      ebs {
        snapshot_id           = var.hosted_data_snapshots[block_device_mappings.key]
        volume_size           = data.aws_ebs_snapshot.hosted[block_device_mappings.key].volume_size
        volume_type           = "gp3"
        encrypted             = true
        delete_on_termination = true
      }
    }
  }

  tags = merge(var.tags, {
    Name                    = "${local.name}-${each.key}-machine"
    "opennova:machine-kind" = each.key
  })

  lifecycle {
    ignore_changes = [image_id]
  }
}

# ---- what the service may do ----------------------------------------------------

locals {
  fleet_arn_prefix = "arn:${local.partition}:ec2:${var.region}"
}

data "aws_iam_policy_document" "fleet_service" {
  count = local.hosted_enabled ? 1 : 0

  # A machine launches only from these templates, as one of their two types
  # with IMDSv2 required (a caller's own --instance-type or metadata options
  # are refused), with this deployment's tags and no other tag.
  statement {
    sid       = "LaunchMachines"
    effect    = "Allow"
    actions   = ["ec2:RunInstances"]
    resources = ["${local.fleet_arn_prefix}:${local.account_id}:instance/*"]

    condition {
      test     = "ArnEquals"
      variable = "ec2:LaunchTemplate"
      values   = [for k in sort(keys(aws_launch_template.fleet)) : aws_launch_template.fleet[k].arn]
    }
    condition {
      test     = "StringEquals"
      variable = "ec2:InstanceType"
      values   = distinct([for k in sort(keys(local.machine_kinds)) : local.machine_kinds[k].instance_type])
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
      values   = local.fleet_tag_keys
    }
  }

  # What such a launch touches besides the machine: its volumes (gp3, no
  # larger than the largest the templates make; IfExists, so a request whose
  # context lacks a key is not refused for that, though the templates name both
  # for every volume), its network interface, the public subnets, the machines'
  # security group, the templates and the snapshots, and the templates' own
  # images. RunInstances on these creates nothing without the machine statement
  # above. The snapshots are encrypted with the AWS-managed aws/ebs key, whose
  # key policy admits every principal of the account through EBS: no KMS grant
  # here.
  statement {
    sid       = "LaunchMachineVolumes"
    effect    = "Allow"
    actions   = ["ec2:RunInstances"]
    resources = ["${local.fleet_arn_prefix}:${local.account_id}:volume/*"]

    condition {
      test     = "StringEqualsIfExists"
      variable = "ec2:VolumeType"
      values   = ["gp3"]
    }
    condition {
      test     = "NumericLessThanEqualsIfExists"
      variable = "ec2:VolumeSize"
      values   = [tostring(local.fleet_max_volume_gb)]
    }
  }

  statement {
    sid       = "LaunchMachineImage"
    effect    = "Allow"
    actions   = ["ec2:RunInstances"]
    resources = ["${local.fleet_arn_prefix}::image/*"]

    # The templates' images by id, each once. Not ec2:Owner: for Canonical's
    # public Ubuntu images EC2 puts the owner alias "amazon" in that key (decoded
    # from a refused launch, 2026-10-14). A template's image_id is the one in
    # state (ignore_changes), so this moves with a -replace of a template alone.
    condition {
      test     = "StringEquals"
      variable = "ec2:ImageID"
      values   = distinct([for k in sort(keys(aws_launch_template.fleet)) : aws_launch_template.fleet[k].image_id])
    }
  }

  statement {
    sid     = "LaunchMachineResources"
    effect  = "Allow"
    actions = ["ec2:RunInstances"]
    resources = concat(
      [
        "${local.fleet_arn_prefix}:${local.account_id}:network-interface/*",
        "${local.fleet_arn_prefix}:${local.account_id}:security-group/${aws_security_group.machines[0].id}",
      ],
      [for k in sort(keys(aws_subnet.public)) : "${local.fleet_arn_prefix}:${local.account_id}:subnet/${aws_subnet.public[k].id}"],
      [for k in sort(keys(aws_launch_template.fleet)) : "${local.fleet_arn_prefix}:${local.account_id}:launch-template/${aws_launch_template.fleet[k].id}"],
      [for id in values(var.hosted_data_snapshots) : "${local.fleet_arn_prefix}::snapshot/${id}"],
    )
  }

  # Tags only as part of a launch.
  statement {
    sid     = "TagMachinesAtLaunch"
    effect  = "Allow"
    actions = ["ec2:CreateTags"]
    resources = [
      "${local.fleet_arn_prefix}:${local.account_id}:instance/*",
      "${local.fleet_arn_prefix}:${local.account_id}:volume/*",
    ]

    condition {
      test     = "StringEquals"
      variable = "ec2:CreateAction"
      values   = ["RunInstances"]
    }
  }

  # Ends only this deployment's machines: both tags.
  statement {
    sid       = "EndMachines"
    effect    = "Allow"
    actions   = ["ec2:TerminateInstances"]
    resources = ["${local.fleet_arn_prefix}:${local.account_id}:instance/*"]

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
    sid       = "DescribeMachines"
    effect    = "Allow"
    actions   = ["ec2:DescribeInstances", "ec2:DescribeLaunchTemplates", "ec2:DescribeLaunchTemplateVersions"]
    resources = ["*"]
  }

  # The machine role, to EC2 alone.
  statement {
    sid       = "PassMachineRole"
    effect    = "Allow"
    actions   = ["iam:PassRole"]
    resources = [aws_iam_role.machine[0].arn]

    condition {
      test     = "StringEquals"
      variable = "iam:PassedToService"
      values   = ["ec2.amazonaws.com"]
    }
  }
}

resource "aws_iam_role_policy" "fleet_service" {
  count  = local.hosted_enabled ? 1 : 0
  name   = "${local.name}-fleet"
  role   = aws_iam_role.service.id
  policy = data.aws_iam_policy_document.fleet_service[0].json
}

# What the service needs to launch and reach the machines, in its infra.env
# (service_ssm.tf). The internal listener's address and the fleet's knobs are
# there whether or not hosting is on; with it off, the provider is `none`. The
# serve and hostd images are the release's (`app deploy --serve-tag
# --hostd-tag`, pinned by digest).
locals {
  fleet_env = {
    ONNET_INTERNAL_HTTP_BIND = "${local.service_private_ip}:${var.internal_http_port}"
    ONNET_FLEET_SHARED_SLOTS = tostring(var.shared_slots)
    ONNET_FLEET_MAX_SHARED   = tostring(var.fleet_max_shared)
    ONNET_FLEET_MAX_OWN      = tostring(var.fleet_max_own)
    ONNET_FLEET_IDLE_MINUTES = tostring(var.fleet_idle_minutes)
  }

  hosted_env = merge(local.fleet_env, local.hosted_enabled ? {
    ONNET_HOSTED_PROVIDER             = "ec2"
    ONNET_HOSTED_EC2_REGION           = var.region
    ONNET_HOSTED_EC2_LAUNCH_TEMPLATES = jsonencode({ for kind, t in aws_launch_template.fleet : kind => t.id })
    ONNET_HOSTED_EC2_SUBNET           = aws_subnet.public["a"].id
    ONNET_HOSTED_DEPLOYMENT           = local.hosted_deployment
    ONNET_HOSTED_BOOTSTRAP_CIDRS      = aws_subnet.public["a"].cidr_block
    # The data sets the service offers are exactly the ones every machine mounts
    # (a volume each, in sorted order), each with its default rotation.
    ONNET_HOSTED_DATA_SETS = jsonencode({
      for slug in sort(keys(var.hosted_data_snapshots)) :
      slug => { missions = lookup(var.hosted_data_set_missions, slug, []) }
    })
    } : {
    ONNET_HOSTED_PROVIDER = "none"
  })
}
