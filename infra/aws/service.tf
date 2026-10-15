# ---------------------------------------------------------------------------
# The service instance (DEPLOY.md, "Operating")
# ---------------------------------------------------------------------------
#
# One EC2 instance runs the NovaWorld stack with docker compose: the server
# (gate UDP 7597, NovaWorld UDP 64206, HTTP 8080; host networking), the
# portal's nginx and the web build (a bridge network, `edge`). It keeps no
# state: secrets and settings live in SSM Parameter Store (service_ssm.tf),
# releases in the operations bucket, logs in CloudWatch. So it is disposable:
# `./deploy/run.sh infra apply --allow-destroy -replace=aws_instance.service`
# makes a new one, which converges itself to the current release at its first
# boot (deploy/host/service-user-data.sh, deploy/host/converge.sh). What
# outlives an instance:
#   - its network interface (aws_network_interface.service), which holds the
#     instance's fixed private address (10.44.0.10 by default): the address the
#     game machines reach the service's internal listener at;
#   - the Elastic IP, associated with that interface. Retail clients find the
#     service by it (gs.novaworld.net, the Switcher's hosts block), so prod
#     adopts its existing address by allocation id
#     (var.service_eip_allocation_id): Terraform only reads it, so no plan can
#     release it. Prod never makes an address, and another deployment never
#     adopts one: each gets an address of its own, managed here. A plan that
#     breaks either rule stops at its precondition.
# The toolbox's `infra apply` also refuses a plan that destroys or replaces
# the instance, its interface, the address or its association unless
# --allow-destroy (deploy/bin/on-deploy, PRECIOUS_TYPES).
# No SSH: no key pair, no port 22. The shell is SSM Session Manager (`app
# shell`), deploys are SSM Run Command (`app deploy`).

data "aws_ami" "ubuntu" {
  count       = var.ami_id == "" ? 1 : 0
  most_recent = true

  owners = ["099720109477"]

  filter {
    name = "name"
    values = [
      "ubuntu/images/hvm-ssd/ubuntu-noble-24.04-amd64-server-*",
      "ubuntu/images/*ubuntu-noble-24.04-amd64-server-*"
    ]
  }

  filter {
    name   = "virtualization-type"
    values = ["hvm"]
  }

  filter {
    name   = "architecture"
    values = ["x86_64"]
  }

  filter {
    name   = "root-device-type"
    values = ["ebs"]
  }

  filter {
    name   = "state"
    values = ["available"]
  }
}

data "aws_caller_identity" "current" {}

data "aws_partition" "current" {}

locals {
  account_id = data.aws_caller_identity.current.account_id
  # Every role the stack makes carries this boundary (infra/aws/iam/opennova-role-boundary.json,
  # made by an admin, outside Terraform): the deploy user may create and change opennova-* roles
  # only under it, so no role it writes can hold more than OpenNova's own resources.
  role_boundary_arn = "arn:${data.aws_partition.current.partition}:iam::${data.aws_caller_identity.current.account_id}:policy/opennova-role-boundary"
  partition         = data.aws_partition.current.partition

  # The host's fixed private address in public subnet a.
  service_private_ip = cidrhost(local.public_subnets.a.cidr, var.service_private_ip_host)

  adopt_eip                 = var.service_eip_allocation_id != ""
  service_eip_allocation_id = local.adopt_eip ? data.aws_eip.service[0].id : aws_eip.service[0].allocation_id
  service_public_ip         = local.adopt_eip ? data.aws_eip.service[0].public_ip : aws_eip.service[0].public_ip
}

# ---- the security group -------------------------------------------------------

# Its description keeps the wording it was made with: AWS fixes a group's
# description for the group's life, so a changed one replaces the group.
resource "aws_security_group" "service" {
  name        = "${local.name}-service"
  description = "OpenNova NovaWorld service host: the retail ports to everyone, the web to Cloudflare alone"
  vpc_id      = aws_vpc.main.id

  tags = merge(var.tags, {
    Name = "${local.name}-service"
  })
}

# Cloudflare's published ranges: the portal is reached through Cloudflare's
# proxy alone, so its port admits nothing else (web/cloudflare-realip.conf
# trusts the same ranges for CF-Connecting-IP).
data "cloudflare_ip_ranges" "edge" {
  count = local.dns_enabled ? 1 : 0
}

locals {
  cloudflare_ipv4 = local.dns_enabled ? sort(data.cloudflare_ip_ranges.edge[0].ipv4_cidr_blocks) : []

  # Every public rule, one per port and range. The retail ports are fixed by
  # the stock clients (gs.novaworld.net's gate on UDP 7597; the gate announces
  # NovaWorld UDP 64206 and http://<address>:8080/ on the same address), so all
  # three stay open on the one Elastic IP; port 8080 stays plain HTTP (the
  # stock NW*.dll and .gsb routes), its /api/ answering the portal's nginx
  # alone (ONNET_API_PROXY_ONLY). The portal's nginx takes Cloudflare's ranges
  # alone, on 443 (edge.tf: the origin certificate). No SSH, no port 80.
  service_ingress = merge(
    { for c in var.allowed_cidrs : "gate ${c}" => { protocol = "udp", port = var.gate_udp_port, cidr = c, description = "novaworld_gate (the stock clients' fixed UDP port)" } },
    { for c in var.allowed_cidrs : "novaworld ${c}" => { protocol = "udp", port = var.nw_udp_port, cidr = c, description = "NovaWorld UDP (the port the gate announces)" } },
    { for c in var.allowed_cidrs : "game-http ${c}" => { protocol = "tcp", port = var.game_http_port, cidr = c, description = "The stock clients' HTTP routes (NW*.dll, .gsb); /api/ answers nginx alone" } },
    { for c in local.cloudflare_ipv4 : "web ${c}" => { protocol = "tcp", port = 443, cidr = c, description = "The portal's nginx over TLS (Full strict), from Cloudflare's proxy alone" } },
  )
}

resource "aws_vpc_security_group_ingress_rule" "service" {
  for_each = local.service_ingress

  security_group_id = aws_security_group.service.id
  description       = each.value.description
  ip_protocol       = each.value.protocol
  from_port         = each.value.port
  to_port           = each.value.port
  cidr_ipv4         = each.value.cidr
}

# Out: the image pulls (ghcr.io, Docker Hub), the AWS APIs, Stripe, the database.
resource "aws_vpc_security_group_egress_rule" "service_all" {
  security_group_id = aws_security_group.service.id
  description       = "All outbound"
  ip_protocol       = "-1"
  cidr_ipv4         = "0.0.0.0/0"
}

# ---- the address --------------------------------------------------------------

resource "aws_network_interface" "service" {
  subnet_id       = aws_subnet.public["a"].id
  private_ips     = [local.service_private_ip]
  security_groups = [aws_security_group.service.id]
  description     = "OpenNova NovaWorld service instance's interface (outlives the instance)"

  tags = merge(var.tags, {
    Name = "${local.name}-service"
  })
}

# Prod's Elastic IP, adopted: read by its allocation id, never managed. Prod
# alone adopts one: another deployment naming an allocation id (a stray
# *.auto.tfvars or TF_VAR_ holding prod's, say) would move that address onto
# its own interface, so its plan stops here, before the address is read.
data "aws_eip" "service" {
  count = local.adopt_eip ? 1 : 0
  id    = var.service_eip_allocation_id

  lifecycle {
    precondition {
      condition     = local.manage_shared
      error_message = "deployment ${local.deployment} names an Elastic IP to adopt (service_eip_allocation_id = ${var.service_eip_allocation_id}): prod alone adopts one, and this plan would move that address onto ${local.deployment}'s interface. Leave service_eip_allocation_id unset off prod (infra/aws/${local.deployment}.tfvars, TF_VAR_service_eip_allocation_id); ${local.deployment} gets an address of its own."
    }
  }
}

# The adopted address carries the deployment's tag like everything the stack
# makes (the provider's default tags reach only resources Terraform manages).
resource "aws_ec2_tag" "service_eip" {
  count       = local.adopt_eip ? 1 : 0
  resource_id = data.aws_eip.service[0].id
  key         = "opennova:deployment"
  value       = local.hosted_deployment
}

# Another deployment gets one of its own. Prod never makes one: its address is
# the one retail clients use, so a prod plan that names none stops here rather
# than give prod a new address (and re-point the association at it).
resource "aws_eip" "service" {
  count  = local.adopt_eip ? 0 : 1
  domain = "vpc"

  tags = merge(var.tags, {
    Name = "${local.name}-eip"
  })

  lifecycle {
    precondition {
      condition     = !local.manage_shared
      error_message = "prod names no Elastic IP to adopt: set service_eip_allocation_id in infra/aws/prod.tfvars to the allocation id of the address retail clients use (./deploy/run.sh aws ec2 describe-addresses). Prod never makes an address of its own; a brand-new prod stack allocates one first (./deploy/run.sh aws ec2 allocate-address --domain vpc) and adopts it (DEPLOY.md, \"The var-file\")."
    }
  }
}

# Associated with the interface, not the instance, so a new instance takes the
# address with the interface. allow_reassociation moves it from wherever it is
# now (the single-host stack's instance, at the cutover); the preconditions
# above keep it to the deployment's own address.
resource "aws_eip_association" "service" {
  allocation_id        = local.service_eip_allocation_id
  network_interface_id = aws_network_interface.service.id
  private_ip_address   = local.service_private_ip
  allow_reassociation  = true
}

# ---- the instance ---------------------------------------------------------------

locals {
  # The first boot's script (deploy/host/service-user-data.sh) with this
  # deployment's values in its @...@ placeholders, and the converge unit it
  # installs.
  service_user_data = replace(replace(replace(replace(replace(replace(
    file("${path.module}/../../deploy/host/service-user-data.sh"),
    "@OPENNOVA_DEPLOYMENT@", local.deployment),
    "@AWS_REGION@", var.region),
    "@OPENNOVA_SSM_PREFIX@", local.ssm_prefix),
    "@OPENNOVA_CONVERGE_DOCUMENT@", aws_ssm_document.converge.name),
    "@OPENNOVA_INTERNAL_PORT@", tostring(var.internal_http_port)),
    "@OPENNOVA_CONVERGE_UNIT@", trimspace(file("${path.module}/../../deploy/host/opennova-converge.service")),
  )
}

resource "aws_instance" "service" {
  ami                  = var.ami_id != "" ? var.ami_id : data.aws_ami.ubuntu[0].id
  instance_type        = var.instance_type
  iam_instance_profile = aws_iam_instance_profile.service.name

  user_data = local.service_user_data
  # A changed first-boot script or converge unit (a comment in either
  # included) is a new instance: the plan shows the replacement, and the
  # toolbox's `infra apply` refuses it without --allow-destroy (aws_instance is
  # one of its precious types; prod also asks for --yes), so it is taken on
  # purpose, at a quiet time. Never false: an in-place user-data change stops
  # and starts the instance, an outage no gate sees, and changes nothing on it
  # (cloud-init runs the script at the first boot alone).
  user_data_replace_on_change = true

  network_interface {
    network_interface_id = aws_network_interface.service.id
    device_index         = 0
  }

  metadata_options {
    http_endpoint = "enabled"
    http_tokens   = "required" # IMDSv2 only
    # One hop: the host's own processes reach the metadata service, no
    # container on a bridge network does (and the host's fence admits root and
    # the server's UID alone: deploy/host/service-user-data.sh).
    http_put_response_hop_limit = 1
    instance_metadata_tags      = "disabled"
  }

  root_block_device {
    volume_type           = "gp3"
    volume_size           = var.service_root_volume_gb
    encrypted             = true
    delete_on_termination = true

    # A root volume takes no provider default tags: the deployment's, spelled out.
    tags = merge(var.tags, {
      Name                  = "${local.name}-service"
      "opennova:deployment" = local.hosted_deployment
    })
  }

  maintenance_options {
    auto_recovery = "default"
  }

  tags = merge(var.tags, {
    Name               = local.name
    "opennova:service" = local.deployment
  })

  # The AMI only seeds a new instance: data.aws_ami.ubuntu is most_recent, so
  # without this every plan after Canonical publishes an image would replace
  # the instance. An OS upgrade is a deliberate replace (infra apply
  # --allow-destroy -replace=aws_instance.service; a changed var.ami_id waits
  # for one too).
  lifecycle {
    ignore_changes = [ami]
  }

  depends_on = [aws_iam_role_policy.service]
}

# ---- the instance role --------------------------------------------------------

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

resource "aws_iam_role" "service" {
  name                 = "${local.name}-service"
  assume_role_policy   = data.aws_iam_policy_document.ec2_assume_role.json
  permissions_boundary = local.role_boundary_arn

  tags = merge(var.tags, {
    Name = "${local.name}-service"
  })
}

locals {
  ops_bucket_arn       = "arn:${local.partition}:s3:::${var.ops_bucket_name}"
  ssm_parameter_prefix = "arn:${local.partition}:ssm:${var.region}:${local.account_id}:parameter${local.ssm_prefix}"
}

# What the host does with its role: read its deployment's parameters (the
# server process reads them too: the metadata fence admits its UID, which the
# hosted machines' EC2 calls need), read its converge document, run SSM's agent
# (Run Command and Session Manager; not the managed AmazonSSMManagedInstanceCore,
# which reads every parameter in the account), read its deployment's releases,
# write its deployment's backups, and ship its containers' logs. fleet.tf adds
# the game machines' launch and end.
data "aws_iam_policy_document" "service" {
  statement {
    sid     = "ReadDeploymentParameters"
    effect  = "Allow"
    actions = ["ssm:GetParameter", "ssm:GetParameters", "ssm:GetParametersByPath"]
    resources = [
      local.ssm_parameter_prefix,
      "${local.ssm_parameter_prefix}/*",
    ]
  }

  statement {
    sid       = "ReadConvergeDocument"
    effect    = "Allow"
    actions   = ["ssm:GetDocument", "ssm:DescribeDocument"]
    resources = [aws_ssm_document.converge.arn]
  }

  statement {
    sid    = "SsmAgent"
    effect = "Allow"
    actions = [
      "ssm:UpdateInstanceInformation",
      "ssm:ListInstanceAssociations",
      "ec2messages:AcknowledgeMessage",
      "ec2messages:DeleteMessage",
      "ec2messages:FailMessage",
      "ec2messages:GetEndpoint",
      "ec2messages:GetMessages",
      "ec2messages:SendReply",
      "ssmmessages:CreateControlChannel",
      "ssmmessages:CreateDataChannel",
      "ssmmessages:OpenControlChannel",
      "ssmmessages:OpenDataChannel",
    ]
    resources = ["*"]
  }

  statement {
    sid       = "ListDeploymentObjects"
    effect    = "Allow"
    actions   = ["s3:ListBucket"]
    resources = [local.ops_bucket_arn]

    condition {
      test     = "StringLike"
      variable = "s3:prefix"
      values   = ["releases/${local.deployment}/*", "${local.backup_prefix}/*"]
    }
  }

  statement {
    sid       = "ReadReleases"
    effect    = "Allow"
    actions   = ["s3:GetObject"]
    resources = ["${local.ops_bucket_arn}/releases/${local.deployment}/*"]
  }

  # Write and read back, never delete: the bucket's lifecycle ends backups.
  statement {
    sid       = "WriteBackups"
    effect    = "Allow"
    actions   = ["s3:PutObject", "s3:GetObject", "s3:AbortMultipartUpload"]
    resources = ["${local.ops_bucket_arn}/${local.backup_prefix}/*"]
  }

  statement {
    sid       = "ShipLogs"
    effect    = "Allow"
    actions   = ["logs:CreateLogStream", "logs:PutLogEvents", "logs:DescribeLogStreams"]
    resources = [aws_cloudwatch_log_group.service.arn, "${aws_cloudwatch_log_group.service.arn}:*"]
  }
}

resource "aws_iam_role_policy" "service" {
  name   = "${local.name}-service"
  role   = aws_iam_role.service.id
  policy = data.aws_iam_policy_document.service.json
}

resource "aws_iam_instance_profile" "service" {
  name = "${local.name}-service"
  role = aws_iam_role.service.name
}
