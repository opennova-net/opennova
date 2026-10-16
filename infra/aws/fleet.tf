# ---------------------------------------------------------------------------
# The fleet: the machines hosted game servers run on (ADR 0053 as amended for
# machines and slots; DEPLOY.md "Hosted game servers")
# ---------------------------------------------------------------------------
#
# The NovaWorld service launches and ends the machines itself (its EC2 machine
# driver, through the AWS CLI, from the worker's jobs, with the worker's task
# role); each runs opennova-hostd, which reaches the controller's internal
# listener by its Cloud Map name (ONNET_INTERNAL_URL), and one opennova-serve
# container per slot. Two kinds:
#   shared  var.shared_instance_type (t3a.large, unlimited CPU credits), up to
#           var.shared_slots servers of any accounts, packed best-fit;
#   own     var.own_instance_type (c7a.medium), exactly one server, the paid
#           own-machine add-on.
# Slot k of a machine: game UDP 32768+k (to players), admin TCP 32800+k (to the
# controller's security group alone). Every machine mounts every data set's
# snapshot (var.hosted_data_snapshots) as a volume of its own, read-only.
#
# Machines launch into both public subnets, so a zone's loss or its want of
# capacity costs a launch its zone, not the fleet (ONNET_HOSTED_EC2_SUBNETS:
# the service picks a subnet per launch).
#
# This file gives the service what it launches from and the least it may do:
#   - the fleet's image: the AMI id in SSM, /opennova/<deployment>/fleet/image,
#     which the templates resolve at each launch (resolve:ssm:). Terraform seeds
#     it with a stock Ubuntu 24.04 image; `./deploy/run.sh fleet bake-image`
#     bakes one with docker installed and the release's serve and hostd images
#     pulled (deploy/host/fleet-image.sh), tagged opennova:fleet-image, and
#     points the parameter at it, so a new machine boots in less time and no
#     plan changes (fleet roll moves running servers onto it);
#   - a launch template per kind: the fleet's image, the kind's type
#     (a burstable one with unlimited credits), an encrypted root volume, one
#     encrypted volume per data set from its private snapshot, the machines'
#     security group, the machine role, IMDSv2 with a hop limit of 1, and
#     InstanceInitiatedShutdownBehavior=terminate (a machine powering off is a
#     machine ended). The service names the template by kind
#     (ONNET_HOSTED_EC2_LAUNCH_TEMPLATES in its environment) and hands each
#     launch its user data;
#   - the machines' security group: the slots' game ports to players, their
#     admin ports to the controller alone; and the controller admitting the
#     machines on its internal port (var.internal_http_port, its second
#     listener, on its task's private address: hostd's bootstrap and sync);
#   - the machine role: the five actions Session Manager needs for break-glass
#     and its logs (/opennova/<deployment>/machines), nothing else (no machine
#     has an SSH key or database credentials);
#   - the worker's task role's policy: launch only from these templates, only their
#     two types with IMDSv2 required, gp3 volumes no larger than the largest
#     the templates make, the seed image or an image this account baked for
#     this deployment, the templates' snapshots, the public subnets, this
#     deployment's tags and no other tag; tag only at launch, end only
#     machines carrying both of this deployment's tags, describe, and pass the
#     machine role to EC2.
#
# All of it exists only while var.hosted_data_snapshots names a snapshot; empty
# (the default), hosting is off and the programs' environment says so.
#
# The ports are the service's and hostd's (apps/novaworld_common/hosted/slots.h):
# change them in both places or in neither.

locals {
  hosted_enabled    = length(var.hosted_data_snapshots) > 0
  hosted_deployment = local.name

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

# The fleet's seed image: Canonical's newest Ubuntu 24.04 when the image
# parameter is made, or var.hosted_ami_id; and its root device's name, which a
# baked image keeps.
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

# ---- the fleet's image ------------------------------------------------------------

# The seed, pinned when it is first planned: later Canonical images move
# neither the parameter (whose value `fleet bake-image` owns) nor the policy's
# seed statement. A new seed is a -replace of this and of the parameter.
resource "terraform_data" "fleet_seed_image" {
  count = local.hosted_enabled ? 1 : 0
  input = data.aws_ami.hosted[0].id

  lifecycle {
    ignore_changes = [input]
  }
}

# What the templates launch from, resolved at each launch. `fleet bake-image`
# writes a baked image's id here; Terraform makes it with the seed and then
# leaves its value alone.
resource "aws_ssm_parameter" "fleet_image" {
  count       = local.hosted_enabled ? 1 : 0
  name        = "${local.ssm_prefix}/fleet/image"
  description = "The image ${local.deployment}'s game machines launch from (./deploy/run.sh fleet bake-image; infra/aws/fleet.tf)"
  type        = "String"
  data_type   = "aws:ec2:image"
  tier        = "Standard"
  value       = terraform_data.fleet_seed_image[0].output

  lifecycle {
    ignore_changes = [value]
  }

  tags = var.tags
}

# ---- network ----------------------------------------------------------------

resource "aws_security_group" "machines" {
  count       = local.hosted_enabled ? 1 : 0
  name        = "${local.name}-machines"
  description = "OpenNova hosted game server machines: slot game ports to players, slot admin ports to the NovaWorld service alone"
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
  description       = "Slot game UDP (slot k: ${local.slot_game_port}+k)"
  ip_protocol       = "udp"
  from_port         = local.slot_game_port
  to_port           = local.slot_game_port + var.shared_slots - 1
  cidr_ipv4         = each.value
}

resource "aws_vpc_security_group_ingress_rule" "machines_admin" {
  count = local.hosted_enabled ? 1 : 0

  security_group_id            = aws_security_group.machines[0].id
  description                  = "Slot remote admin from the NovaWorld controller alone (slot k: ${local.slot_admin_port}+k)"
  ip_protocol                  = "tcp"
  from_port                    = local.slot_admin_port
  to_port                      = local.slot_admin_port + var.shared_slots - 1
  referenced_security_group_id = aws_security_group.program["controller"].id
}

# Out: the image pulls (ghcr.io), Ubuntu's mirrors, the controller's internal
# port, and NovaWorld at the gate's public address.
resource "aws_vpc_security_group_egress_rule" "machines_all" {
  count = local.hosted_enabled ? 1 : 0

  security_group_id = aws_security_group.machines[0].id
  description       = "All outbound"
  ip_protocol       = "-1"
  cidr_ipv4         = "0.0.0.0/0"
}

# The controller admits the machines on its internal port alone: its second
# listener, on its task's private address (the leader alone binds it), where
# hostd fetches its bootstrap and syncs.
resource "aws_vpc_security_group_ingress_rule" "controller_from_machines" {
  count = local.hosted_enabled ? 1 : 0

  security_group_id            = aws_security_group.program["controller"].id
  description                  = "Machine hostd: bootstrap and sync on the controller internal listener (private)"
  ip_protocol                  = "tcp"
  from_port                    = var.internal_http_port
  to_port                      = var.internal_http_port
  referenced_security_group_id = aws_security_group.machines[0].id
}

# ---- the machine role ---------------------------------------------------------

resource "aws_iam_role" "machine" {
  count                = local.hosted_enabled ? 1 : 0
  name                 = "${local.name}-machine"
  assume_role_policy   = data.aws_iam_policy_document.ec2_assume_role.json
  permissions_boundary = local.role_boundary_arn

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

  image_id                             = "resolve:ssm:${aws_ssm_parameter.fleet_image[0].name}"
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
}

# ---- what the service may do ----------------------------------------------------

locals {
  fleet_arn_prefix = "arn:${local.partition}:ec2:${local.region}"
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

  # The seed image, by id. Not ec2:Owner: for Canonical's public Ubuntu
  # images EC2 puts the owner alias "amazon" in that key (decoded from a
  # refused launch, 2026-10-14). The pinned seed (terraform_data), so this
  # moves with a -replace of it alone.
  statement {
    sid       = "LaunchMachineSeedImage"
    effect    = "Allow"
    actions   = ["ec2:RunInstances"]
    resources = ["${local.fleet_arn_prefix}::image/*"]

    condition {
      test     = "StringEquals"
      variable = "ec2:ImageID"
      values   = [terraform_data.fleet_seed_image[0].output]
    }
  }

  # An image `fleet bake-image` made: this account's, tagged as this
  # deployment's fleet image.
  statement {
    sid       = "LaunchMachineBakedImage"
    effect    = "Allow"
    actions   = ["ec2:RunInstances"]
    resources = ["${local.fleet_arn_prefix}::image/*"]

    condition {
      test     = "StringEquals"
      variable = "ec2:Owner"
      values   = [local.account_id]
    }
    condition {
      test     = "StringEquals"
      variable = "aws:ResourceTag/opennova:fleet-image"
      values   = [local.hosted_deployment]
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

  # The templates name their image by SSM parameter (resolve:ssm:), which
  # RunInstances reads as its caller: without it every launch fails
  # SsmAccessDenied.
  statement {
    sid       = "ResolveFleetImage"
    effect    = "Allow"
    actions   = ["ssm:GetParameters"]
    resources = [aws_ssm_parameter.fleet_image[0].arn]
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

# The worker runs the jobs that launch, describe and end machines.
resource "aws_iam_role_policy" "fleet_service" {
  count  = local.hosted_enabled ? 1 : 0
  name   = "${local.name}-fleet"
  role   = aws_iam_role.program["worker"].id
  policy = data.aws_iam_policy_document.fleet_service[0].json
}

# What web, the controller and the worker need to launch and reach the
# machines, in their environment (programs.tf). The fleet's knobs are there
# whether or not hosting is on (the controller's address is ONNET_INTERNAL_URL,
# and its task binds its own private address); with it off, the provider is
# `none`. The serve and hostd images are the release's (`app deploy
# --serve-tag --hostd-tag`, pinned by digest). The toolbox's own (infra.env):
# what `fleet bake-image` and the machines' tag need.
locals {
  fleet_env = {
    ONNET_FLEET_SHARED_SLOTS = tostring(var.shared_slots)
    ONNET_FLEET_MAX_SHARED   = tostring(var.fleet_max_shared)
    ONNET_FLEET_MAX_OWN      = tostring(var.fleet_max_own)
    ONNET_FLEET_IDLE_MINUTES = tostring(var.fleet_idle_minutes)
  }

  hosted_env = merge(local.fleet_env, local.hosted_enabled ? {
    ONNET_HOSTED_PROVIDER             = "ec2"
    ONNET_HOSTED_EC2_REGION           = local.region
    ONNET_HOSTED_EC2_LAUNCH_TEMPLATES = jsonencode({ for kind, t in aws_launch_template.fleet : kind => t.id })
    # Both public subnets, comma-separated: the service picks one per launch.
    ONNET_HOSTED_EC2_SUBNETS     = join(",", [for k in sort(keys(aws_subnet.public)) : aws_subnet.public[k].id])
    ONNET_HOSTED_DEPLOYMENT      = local.hosted_deployment
    ONNET_HOSTED_BOOTSTRAP_CIDRS = join(",", [for k in sort(keys(aws_subnet.public)) : aws_subnet.public[k].cidr_block])
    # The data sets the service offers are exactly the ones every machine mounts
    # (a volume each, in sorted order), each with its default rotation.
    ONNET_HOSTED_DATA_SETS = jsonencode({
      for slug in sort(keys(var.hosted_data_snapshots)) :
      slug => { missions = lookup(var.hosted_data_set_missions, slug, []) }
    })
    } : {
    ONNET_HOSTED_PROVIDER = "none"
  })

  fleet_toolbox_env = merge(
    {
      ONNET_HOSTED_PROVIDER   = local.hosted_env.ONNET_HOSTED_PROVIDER
      ONNET_HOSTED_DEPLOYMENT = local.hosted_deployment
    },
    local.hosted_enabled ? {
      ONNET_HOSTED_EC2_SUBNETS = local.hosted_env.ONNET_HOSTED_EC2_SUBNETS
      # What `fleet bake-image` launches its builder with and writes its image to.
      OPENNOVA_FLEET_IMAGE_PARAMETER = aws_ssm_parameter.fleet_image[0].name
      OPENNOVA_FLEET_SECURITY_GROUP  = aws_security_group.machines[0].id
    } : {},
  )
}
