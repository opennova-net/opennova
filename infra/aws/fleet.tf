# ---------------------------------------------------------------------------
# The fleet: the machines hosted game servers run on (ADR 0053 as amended for
# machines and slots; DEPLOY.md "Hosted game servers")
# ---------------------------------------------------------------------------
#
# The NovaWorld service launches and ends the machines itself (its EC2 machine
# driver, through the AWS CLI, from the worker's jobs, with the worker's task
# role); each registers in the fleet's ECS cluster, which runs opennova-hostd
# on it (fleet_agent.tf), and hostd reaches the fleet program's internal
# listener by its Cloud Map name and runs one opennova-serve
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
# the trees its servers pin on demand (`aws s3 sync`, under hostd's task role)
# onto the root volume, /opt/game/<tree id>/, and keeps each while a server
# needs it.
#
# Machines launch into both public subnets, so a zone's loss or its want of
# capacity costs a launch its zone, not the fleet (ONNET_HOSTED_EC2_SUBNETS:
# the service picks a subnet per launch).
#
# This file gives the service what it launches from and the least it may do:
#   - a launch template per kind: the fleet's image (the deployment's pinned
#     ECS-optimized Amazon Linux 2023 AMI, var.fleet_ami, fleet_agent.tf), the
#     kind's type (a burstable one with unlimited credits), an encrypted root
#     volume (the trees live there), the machines' security group, the machine
#     role, IMDSv2 with a hop limit of 1, and
#     InstanceInitiatedShutdownBehavior=terminate (a machine powering off is a
#     machine ended). The service names the template by kind
#     (ONNET_HOSTED_EC2_LAUNCH_TEMPLATES in its environment) and hands each
#     launch its user data: the token and machine-id files, the slots'
#     reserved ports and /etc/ecs/ecs.config, which registers the machine in
#     the fleet cluster (apps/novaworld_common/hosted/machine/);
#   - the machines' security group: the slots' game ports to players, their
#     admin ports to the fleet program alone; and the fleet program admitting
#     the machines on its internal port (var.internal_http_port, its second
#     listener, on its task's private address: hostd's bootstrap and sync);
#   - the machine role, the instance's: the ECS agent's calls on the fleet
#     cluster (fleet_agent.tf runs hostd there, a DAEMON service, one task per
#     machine) and the logs docker's awslogs driver writes with it (hostd's
#     own, /opennova/<deployment>/machines, stream hostd/hostd/<task id>; the
#     slots' containers'), nothing else (no machine has an SSH key, a Session
#     Manager agent's rights or database credentials; the trees are hostd's
#     task role's);
#   - the worker's task role's policy: launch only from these templates, only their
#     two types with IMDSv2 required, gp3 volumes no larger than the root
#     volume the templates make, the pinned image, the public subnets, this
#     deployment's tags and no other tag; tag only at launch, end only
#     machines carrying both of this deployment's tags, describe, and pass the
#     machine role to EC2; and the static addresses (the own machines'
#     add-on): allocate an Elastic IP only with this deployment's tags and no
#     other tag, tag it only as it is allocated, associate one carrying both
#     tags only with a machine carrying both, disassociate and release only
#     those, and (while edge.tf's play zone exists) write and delete A records
#     under that zone alone.
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
  # And the only tags an allocated static address may carry.
  address_tag_keys = ["opennova:hosted", "opennova:deployment", "opennova:address-id", "Name"]

  # The largest volume a launch may make: the root volume (the one volume the
  # templates make).
  fleet_max_volume_gb = max(16, var.machine_root_volume_gb)
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

# Out: the image pulls (ghcr.io), the ECS and CloudWatch Logs endpoints, the
# store, the fleet program's internal port, and NovaWorld at the gate's public
# address.
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
  permissions_boundary = local.machine_boundary_arn

  tags = merge(var.tags, {
    Name = "${local.name}-machine"
  })
}

# The ECS agent's calls (the AL2023 ECS-optimized image runs it): registering
# the machine as a container instance of the fleet cluster and its task and
# container states, polling for its work and its telemetry session, on the
# fleet cluster and its instances alone (DiscoverPollEndpoint takes no
# resource); and the machine's logs, which the docker daemon's awslogs driver
# makes each stream of and writes with this role: hostd's own (its task has no
# execution role) and the slots' containers'. The servers' group is named, not
# referenced: observability.tf makes it. hostd's task role reads the trees; a
# slot's user reaches neither role's credentials (hostd's fences). Both roles
# carry the machine boundary (infra/aws/iam/opennova-machine-boundary.json),
# which holds what they do and nothing of the programs'.
data "aws_iam_policy_document" "machine" {
  count = local.hosted_enabled ? 1 : 0

  statement {
    sid    = "EcsAgentCluster"
    effect = "Allow"
    actions = [
      "ecs:RegisterContainerInstance",
      "ecs:DeregisterContainerInstance",
      "ecs:SubmitContainerStateChange",
      "ecs:SubmitTaskStateChange",
      "ecs:SubmitAttachmentStateChanges",
    ]
    resources = [local.fleet_cluster_arn]
  }

  statement {
    sid       = "EcsAgentInstance"
    effect    = "Allow"
    actions   = ["ecs:Poll", "ecs:StartTelemetrySession"]
    resources = ["arn:${local.partition}:ecs:${local.region}:${local.account_id}:container-instance/${local.fleet_cluster_name}/*"]
  }

  statement {
    sid       = "EcsAgentDiscover"
    effect    = "Allow"
    actions   = ["ecs:DiscoverPollEndpoint"]
    resources = ["*"]
  }

  statement {
    sid     = "ShipMachineLogs"
    effect  = "Allow"
    actions = ["logs:CreateLogStream", "logs:PutLogEvents"]
    resources = [
      aws_cloudwatch_log_group.machines.arn, "${aws_cloudwatch_log_group.machines.arn}:*",
      local.servers_log_group_arn, "${local.servers_log_group_arn}:*",
    ]
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

  image_id                             = data.aws_ami.fleet[0].id
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

  # The root volume, the machine's one volume: the OS, docker, hostd's and
  # the serve images, the slots' working directories (their secret files
  # among them) and the trees hostd fetches (/opt/game).
  block_device_mappings {
    device_name = data.aws_ami.fleet[0].root_device_name

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
  # templates, and the templates' image. RunInstances on these creates
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

  # The pinned image alone, by id (fleet_agent.tf's data.aws_ami.fleet):
  # a bump of var.fleet_ami moves this with the templates, in one plan.
  statement {
    sid       = "LaunchMachineImage"
    effect    = "Allow"
    actions   = ["ec2:RunInstances"]
    resources = ["${local.fleet_arn_prefix}::image/*"]

    condition {
      test     = "StringEquals"
      variable = "ec2:ImageID"
      values   = [data.aws_ami.fleet[0].id]
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

  # A static address is allocated with this deployment's tags and no other
  # tag (the service's allocate-address names them in its tag
  # specifications), so the statements below know it by them.
  statement {
    sid       = "AllocateAddresses"
    effect    = "Allow"
    actions   = ["ec2:AllocateAddress"]
    resources = ["${local.fleet_arn_prefix}:${local.account_id}:elastic-ip/*"]

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
      values   = local.address_tag_keys
    }
  }

  # Tags only as part of an allocation.
  statement {
    sid       = "TagAddressesAtAllocation"
    effect    = "Allow"
    actions   = ["ec2:CreateTags"]
    resources = ["${local.fleet_arn_prefix}:${local.account_id}:elastic-ip/*"]

    condition {
      test     = "StringEquals"
      variable = "ec2:CreateAction"
      values   = ["AllocateAddress"]
    }
  }

  # Moves and releases only the addresses carrying both of this deployment's
  # tags: never the load balancer's (they carry the deployment's tag alone).
  statement {
    sid       = "AssociateReleaseAddresses"
    effect    = "Allow"
    actions   = ["ec2:AssociateAddress", "ec2:DisassociateAddress", "ec2:ReleaseAddress"]
    resources = ["${local.fleet_arn_prefix}:${local.account_id}:elastic-ip/*"]

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

  # An association names its machine, which carries both tags too.
  statement {
    sid       = "AssociateAddressesWithMachines"
    effect    = "Allow"
    actions   = ["ec2:AssociateAddress"]
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

  # No resource scope exists for it.
  statement {
    sid       = "DescribeAddresses"
    effect    = "Allow"
    actions   = ["ec2:DescribeAddresses"]
    resources = ["*"]
  }

  # The addresses' names, while edge.tf's play zone exists: A records under
  # it alone, made or replaced (UPSERT) and deleted, and nothing else of it.
  dynamic "statement" {
    for_each = aws_route53_zone.play[*].arn
    content {
      sid       = "WriteAddressRecords"
      effect    = "Allow"
      actions   = ["route53:ChangeResourceRecordSets"]
      resources = [statement.value]

      condition {
        test     = "ForAllValues:StringEquals"
        variable = "route53:ChangeResourceRecordSetsRecordTypes"
        values   = ["A"]
      }
      condition {
        test     = "ForAllValues:StringEquals"
        variable = "route53:ChangeResourceRecordSetsActions"
        values   = ["UPSERT", "DELETE"]
      }
      condition {
        test     = "ForAllValues:StringLike"
        variable = "route53:ChangeResourceRecordSetsNormalizedRecordNames"
        values   = ["*.${local.play_zone_name}"]
      }
    }
  }

  # A change's state, while the play zone exists (a release waits for its
  # DELETE to be INSYNC before it gives the address back).
  dynamic "statement" {
    for_each = aws_route53_zone.play[*].arn
    content {
      sid       = "ReadAddressRecordChanges"
      effect    = "Allow"
      actions   = ["route53:GetChange"]
      resources = ["arn:${local.partition}:route53:::change/*"]
    }
  }
}

# The worker runs the jobs that launch, describe and end machines, and those
# that allocate, associate and release the static addresses.
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
# machines into the fleet cluster). The store is the policy's and the data
# program's too. The fleet's knobs are there whether or not hosting is on; with
# it off, the provider is `none` and there is no store or driver. The serve
# image is the release's (`app deploy --serve-tag`, pinned by digest); hostd's
# is its task definition's (fleet_agent.tf), not an environment's. The
# toolbox's own (infra.env): the machines' tag, the fleet cluster and the image
# name it is pinned to.
locals {
  fleet_env = {
    ONNET_FLEET_SHARED_SLOTS  = tostring(var.shared_slots)
    ONNET_FLEET_MAX_SHARED    = tostring(var.fleet_max_shared)
    ONNET_FLEET_MAX_OWN       = tostring(var.fleet_max_own)
    ONNET_FLEET_MAX_ADDRESSES = tostring(var.fleet_max_addresses)
    ONNET_FLEET_IDLE_MINUTES  = tostring(var.fleet_idle_minutes)
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

  # The fleet's policy, with the static addresses' zone (edge.tf) while it
  # exists: the name each address is shown at, <name>.<zone>.
  hosted_policy_env = merge(local.fleet_env, local.store_env, local.play_zone_enabled ? {
    ONNET_HOSTED_ADDRESS_ZONE = local.play_zone_name
    } : {}, local.hosted_enabled ? {
    ONNET_HOSTED_PROVIDER        = "ec2"
    ONNET_HOSTED_BOOTSTRAP_CIDRS = join(",", [for k in sort(keys(aws_subnet.public)) : aws_subnet.public[k].cidr_block])
    # The image the templates launch (var.fleet_ami's id): what a fleet roll's
    # preview compares each machine's against.
    ONNET_FLEET_IMAGE_ID = data.aws_ami.fleet[0].id
    } : {
    ONNET_HOSTED_PROVIDER = "none"
  })

  # The ec2 machine driver's own settings, the worker's, and the static
  # addresses' driver's while the play zone exists: the zone's id, where the
  # worker writes each address's A record.
  hosted_driver_env = merge(local.hosted_enabled ? {
    ONNET_HOSTED_EC2_REGION           = local.region
    ONNET_HOSTED_EC2_LAUNCH_TEMPLATES = jsonencode({ for kind, t in aws_launch_template.fleet : kind => t.id })
    # The fleet cluster a machine's ecs.config names (its user data), which
    # runs hostd on it.
    ONNET_HOSTED_EC2_CLUSTER = aws_ecs_cluster.fleet[0].name
    # Both public subnets, comma-separated: the service picks one per launch.
    ONNET_HOSTED_EC2_SUBNETS = join(",", [for k in sort(keys(aws_subnet.public)) : aws_subnet.public[k].id])
    ONNET_HOSTED_DEPLOYMENT  = local.hosted_deployment
    ONNET_INTERNAL_URL       = local.internal_url
    } : {}, local.play_zone_enabled ? {
    ONNET_HOSTED_ADDRESS_ZONE_ID = aws_route53_zone.play[0].zone_id
  } : {})

  # The fleet cluster and the image name it is pinned to: "" while hosting
  # is off (the toolbox then has no hostd to roll).
  fleet_toolbox_env = {
    ONNET_HOSTED_PROVIDER   = local.hosted_policy_env.ONNET_HOSTED_PROVIDER
    ONNET_HOSTED_DEPLOYMENT = local.hosted_deployment
    OPENNOVA_FLEET_CLUSTER  = local.hosted_enabled ? aws_ecs_cluster.fleet[0].name : ""
    OPENNOVA_FLEET_AMI_NAME = local.hosted_enabled ? var.fleet_ami : ""
  }
}
