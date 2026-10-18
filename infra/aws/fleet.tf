# ---------------------------------------------------------------------------
# The fleet: the machines hosted game servers run on (ADR 0053 as amended for
# machines and slots; DEPLOY.md "Hosted game servers")
# ---------------------------------------------------------------------------
#
# The NovaWorld service launches and ends the machines itself (its EC2 machine
# driver, through the AWS CLI, from the worker's jobs, with the worker's task
# role); each runs opennova-hostd, which reaches the fleet program's internal
# listener by its Cloud Map name (ONNET_INTERNAL_URL), and one opennova-serve
# container per slot. Two kinds:
#   shared  var.shared_instance_type (t3a.medium, unlimited CPU credits), up to
#           var.shared_slots servers of any accounts, packed best-fit;
#   own     var.own_instance_type (c7a.medium), exactly one server, the paid
#           own-machine add-on.
# Slot k of a machine: game UDP 32768+k (to players), admin TCP 32800+k (to the
# fleet program's security group alone). A server's game data is a tree (one
# version of a data set's files, ADR 0064) in the store: the deployment's
# operations bucket, data/trees/<id>/, which admins fill from the site's Game
# data page. A machine holds no volume of game data: its hostd fetches
# the trees its servers pin on demand (`aws s3 sync`, under the machine role)
# onto the root volume, /opt/game/<tree id>/, and keeps each while a server
# needs it.
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
#     (a burstable one with unlimited credits), an encrypted root volume (the
#     trees live there), the machines'
#     security group, the machine role, IMDSv2 with a hop limit of 1, and
#     InstanceInitiatedShutdownBehavior=terminate (a machine powering off is a
#     machine ended). The service names the template by kind
#     (ONNET_HOSTED_EC2_LAUNCH_TEMPLATES in its environment) and hands each
#     launch its user data;
#   - the machines' security group: the slots' game ports to players, their
#     admin ports to the fleet program alone; and the fleet program admitting
#     the machines on its internal port (var.internal_http_port, its second
#     listener, on its task's private address: hostd's bootstrap and sync);
#   - the machine role: the five actions Session Manager needs for break-glass,
#     its logs (/opennova/<deployment>/machines, which docker's awslogs
#     driver writes: a stream per slot, <machine id>/slot-<k>, and the agent's,
#     <machine id>/hostd; the service hands each machine the group,
#     ONNET_HOSTED_EC2_LOG_GROUP), and a read of the trees in the store,
#     nothing else (no machine has an SSH key or database credentials);
#   - the worker's task role's policy: launch only from these templates, only their
#     two types with IMDSv2 required, gp3 volumes no larger than the root
#     volume the templates make, the seed image or an image this account baked
#     for this deployment, the public subnets, this deployment's tags and no
#     other tag; tag only at launch, end only machines carrying both of this
#     deployment's tags, describe, and pass the machine role to EC2.
#
# All of it exists only while var.hosted_enabled is on; off (the default),
# hosting is off and the programs' environment says so. Web's and the data
# program's own store policies are programs.tf's.
#
# The ports are the service's and hostd's (apps/novaworld_common/hosted/slots.h):
# change them in both places or in neither.

locals {
  hosted_enabled    = var.hosted_enabled
  hosted_deployment = local.name

  # The game data's store: the deployment's operations bucket under data/
  # (trees/<tree id>/<file> and updates/<sha256>.sfs,
  # apps/novaworld_common/object_store.h), as the programs name it
  # (ONNET_OBJECT_STORE_URL) and the policies scope it. The bucket is the
  # deployment's own (ops_bucket.tf), so the prefix carries no deployment.
  data_prefix      = "data"
  object_store_url = "s3://${local.ops_bucket_name}/${local.data_prefix}"

  slot_game_port  = 32768
  slot_admin_port = 32800

  machine_kinds = {
    shared = { instance_type = var.shared_instance_type, slots = var.shared_slots }
    own    = { instance_type = var.own_instance_type, slots = 1 }
  }

  # The only tags a launch may carry (the service's run-instances names these).
  fleet_tag_keys = ["opennova:hosted", "opennova:deployment", "opennova:machine-kind", "opennova:machine-id", "opennova:server-id", "Name"]

  # The largest volume a launch may make: the root volume (the one volume the
  # templates make).
  fleet_max_volume_gb = max(16, var.machine_root_volume_gb)
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
  description                  = "Slot remote admin from the NovaWorld fleet program alone (slot k: ${local.slot_admin_port}+k)"
  ip_protocol                  = "tcp"
  from_port                    = local.slot_admin_port
  to_port                      = local.slot_admin_port + var.shared_slots - 1
  referenced_security_group_id = aws_security_group.program["fleet"].id
}

# Out: the image pulls (ghcr.io), Ubuntu's mirrors, the fleet program's
# internal port, and NovaWorld at the gate's public address.
resource "aws_vpc_security_group_egress_rule" "machines_all" {
  count = local.hosted_enabled ? 1 : 0

  security_group_id = aws_security_group.machines[0].id
  description       = "All outbound"
  ip_protocol       = "-1"
  cidr_ipv4         = "0.0.0.0/0"
}

# The fleet program admits the machines on its internal port alone: its second
# listener, on its task's private address (the leader alone binds it), where
# hostd fetches its bootstrap and syncs.
resource "aws_vpc_security_group_ingress_rule" "fleet_from_machines" {
  count = local.hosted_enabled ? 1 : 0

  security_group_id            = aws_security_group.program["fleet"].id
  description                  = "Machine hostd: bootstrap and sync on the fleet internal listener (private)"
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
# registration and its session channels; the machine's logs (the docker
# daemon's awslogs driver makes each stream and writes it); and the trees
# hostd fetches. Not the managed AmazonSSMManagedInstanceCore, which also
# reads every SSM parameter in the account. (The machine also fences the
# slots' UIDs off the metadata service, so no server holds even these.)
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

  # The trees (immutable, written by the worker): hostd's `aws s3 sync` lists
  # a tree's prefix and reads its objects. The trees alone, read alone:
  # nothing of the dumps, the releases or the uploads.
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

  # The root volume, the machine's one volume: the OS, docker, the serve
  # image, the slots' working directories (their secret files among them) and
  # the trees hostd fetches (/opt/game).
  block_device_mappings {
    device_name = data.aws_ami.hosted[0].root_device_name

    ebs {
      volume_size           = var.machine_root_volume_gb
      volume_type           = "gp3"
      encrypted             = true
      delete_on_termination = true
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

  # What such a launch touches besides the machine: its root volume (gp3, no
  # larger than the templates make; IfExists, so a request whose context
  # lacks a key is not refused for that, though the templates name both), its
  # network interface, the public subnets, the machines' security group, the
  # templates, and the templates' own images. RunInstances on these creates
  # nothing without the machine statement above. The volume is encrypted with
  # the AWS-managed aws/ebs key, whose key policy admits every principal of
  # the account through EBS: no KMS grant here.
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

# What the programs need of the fleet, in their environment (programs.tf), in
# the service's two sets (apps/novaworld_common/hosted/fleet_config.h): the
# fleet's policy, which web, the fleet program and the worker read alike, and
# the machine driver's own settings, the worker's alone (it alone launches the
# machines; the fleet program's address it hands them is ONNET_INTERNAL_URL,
# and that task binds its own private address). The store is the policy's and
# the data program's too. The fleet's knobs are there whether or not hosting is
# on; with it off, the provider is `none` and there is no store or driver. The
# serve and hostd images are the release's (`app deploy --serve-tag
# --hostd-tag`, pinned by digest). The toolbox's own (infra.env): what `fleet
# bake-image` and the machines' tag need.
locals {
  fleet_env = {
    ONNET_FLEET_SHARED_SLOTS = tostring(var.shared_slots)
    ONNET_FLEET_MAX_SHARED   = tostring(var.fleet_max_shared)
    ONNET_FLEET_MAX_OWN      = tostring(var.fleet_max_own)
    ONNET_FLEET_IDLE_MINUTES = tostring(var.fleet_idle_minutes)
    # The live servers the service takes: what the fleet holds at its most.
    ONNET_HOSTED_CAP = tostring(var.fleet_max_shared * var.shared_slots + var.fleet_max_own)
  }

  # The game data's store: the trees the machines fetch (the fleet program
  # hands hostd each tree's URL under it), what web uploads and the data
  # program builds.
  store_env = local.hosted_enabled ? {
    ONNET_OBJECT_STORE_URL    = local.object_store_url
    ONNET_OBJECT_STORE_REGION = var.region
  } : {}

  # The fleet's policy.
  hosted_policy_env = merge(local.fleet_env, local.store_env, local.hosted_enabled ? {
    ONNET_HOSTED_PROVIDER        = "ec2"
    ONNET_HOSTED_BOOTSTRAP_CIDRS = join(",", [for k in sort(keys(aws_subnet.public)) : aws_subnet.public[k].cidr_block])
    } : {
    ONNET_HOSTED_PROVIDER = "none"
  })

  # The ec2 machine driver's own settings: the worker's.
  hosted_driver_env = local.hosted_enabled ? {
    ONNET_HOSTED_EC2_REGION           = local.region
    ONNET_HOSTED_EC2_LOG_GROUP        = aws_cloudwatch_log_group.machines.name
    ONNET_HOSTED_EC2_LAUNCH_TEMPLATES = jsonencode({ for kind, t in aws_launch_template.fleet : kind => t.id })
    # Both public subnets, comma-separated: the service picks one per launch.
    ONNET_HOSTED_EC2_SUBNETS = join(",", [for k in sort(keys(aws_subnet.public)) : aws_subnet.public[k].id])
    ONNET_HOSTED_DEPLOYMENT  = local.hosted_deployment
    ONNET_INTERNAL_URL       = local.internal_url
  } : {}

  fleet_toolbox_env = merge(
    {
      ONNET_HOSTED_PROVIDER   = local.hosted_policy_env.ONNET_HOSTED_PROVIDER
      ONNET_HOSTED_DEPLOYMENT = local.hosted_deployment
    },
    local.hosted_enabled ? {
      ONNET_HOSTED_EC2_SUBNETS = local.hosted_driver_env.ONNET_HOSTED_EC2_SUBNETS
      # What `fleet bake-image` launches its builder with and writes its image to.
      OPENNOVA_FLEET_IMAGE_PARAMETER = aws_ssm_parameter.fleet_image[0].name
      OPENNOVA_FLEET_SECURITY_GROUP  = aws_security_group.machines[0].id
    } : {},
  )
}
