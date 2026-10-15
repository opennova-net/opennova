# ---------------------------------------------------------------------------
# The service host: one EC2 instance running the NovaWorld stack (docker
# compose), its security group, its Elastic IP and its instance role.
# ---------------------------------------------------------------------------

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

locals {
  public_ip = var.allocate_eip ? aws_eip.server[0].public_ip : aws_instance.server.public_ip
}

resource "aws_key_pair" "server" {
  key_name   = "${local.name}-key"
  public_key = var.ssh_public_key

  tags = merge(var.tags, {
    Name = "${local.name}-key"
  })
}

resource "aws_security_group" "server" {
  name        = "${local.name}-sg"
  description = "Security group for the OpenNova NovaWorld services"
  vpc_id      = aws_vpc.main.id

  ingress {
    description = "SSH"
    from_port   = 22
    to_port     = 22
    protocol    = "tcp"
    cidr_blocks = [var.ssh_allowed_cidr]
  }

  dynamic "ingress" {
    for_each = var.allowed_cidrs
    content {
      description = "novaworld_gate UDP"
      from_port   = var.gate_udp_port
      to_port     = var.gate_udp_port
      protocol    = "udp"
      cidr_blocks = [ingress.value]
    }
  }

  dynamic "ingress" {
    for_each = var.allowed_cidrs
    content {
      description = "NovaWorld UDP"
      from_port   = var.nw_udp_port
      to_port     = var.nw_udp_port
      protocol    = "udp"
      cidr_blocks = [ingress.value]
    }
  }

  dynamic "ingress" {
    for_each = var.allowed_cidrs
    content {
      description = "Game HTTP"
      from_port   = var.game_http_port
      to_port     = var.game_http_port
      protocol    = "tcp"
      cidr_blocks = [ingress.value]
    }
  }

  # The hosted game server machines (hosted.tf) fetch their bootstrap bundle
  # from this port over the private network; the service answers only their
  # subnet there (ONNET_HOSTED_BOOTSTRAP_CIDRS). No rule while hosting is off.
  dynamic "ingress" {
    for_each = aws_security_group.hosted[*].id
    content {
      description     = "Hosted machines bootstrap fetch (private)"
      from_port       = var.game_http_port
      to_port         = var.game_http_port
      protocol        = "tcp"
      security_groups = [ingress.value]
    }
  }

  dynamic "ingress" {
    for_each = var.allowed_cidrs
    content {
      description = "Web Frontend"
      from_port   = var.web_http_port
      to_port     = var.web_http_port
      protocol    = "tcp"
      cidr_blocks = [ingress.value]
    }
  }

  dynamic "ingress" {
    for_each = var.allowed_cidrs
    content {
      description = "Web Frontend HTTPS"
      from_port   = var.web_https_port
      to_port     = var.web_https_port
      protocol    = "tcp"
      cidr_blocks = [ingress.value]
    }
  }

  egress {
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }

  tags = merge(var.tags, {
    Name = local.name
  })
}

resource "aws_instance" "server" {
  ami                         = var.ami_id != "" ? var.ami_id : data.aws_ami.ubuntu[0].id
  instance_type               = var.instance_type
  subnet_id                   = aws_subnet.public["a"].id
  vpc_security_group_ids      = [aws_security_group.server.id]
  key_name                    = aws_key_pair.server.key_name
  associate_public_ip_address = true
  iam_instance_profile        = aws_iam_instance_profile.backup.name

  user_data = file("${path.module}/user_data.sh.tmpl")

  metadata_options {
    http_tokens                 = "required" # IMDSv2 only
    http_put_response_hop_limit = 2          # containers reach IMDS for backup creds
  }

  tags = merge(var.tags, {
    Name = local.name
  })

  # The AMI only seeds a new instance. data.aws_ami.ubuntu is most_recent, so
  # without this every plan after Canonical publishes an image would replace
  # the live server (downtime, and the instance's sqlite DB gone). An OS
  # upgrade is a deliberate replace: terraform apply -replace=aws_instance.server
  # (a changed var.ami_id also waits for that).
  lifecycle {
    ignore_changes = [ami]
  }
}

resource "aws_eip" "server" {
  count    = var.allocate_eip ? 1 : 0
  instance = aws_instance.server.id

  tags = merge(var.tags, {
    Name = "${local.name}-eip"
  })
}

# ---------------------------------------------------------------------------
# The instance role (the backups bucket; hosted.tf adds the machines' policy)
# ---------------------------------------------------------------------------

data "aws_iam_policy_document" "backup_assume_role" {
  statement {
    effect = "Allow"

    principals {
      type        = "Service"
      identifiers = ["ec2.amazonaws.com"]
    }

    actions = ["sts:AssumeRole"]
  }
}

resource "aws_iam_role" "backup" {
  name               = "${local.name}-backup-role"
  assume_role_policy = data.aws_iam_policy_document.backup_assume_role.json

  tags = merge(var.tags, {
    Name = "${local.name}-backup-role"
  })
}

# Built from the bucket NAME (not the resource) so the instance role can
# reference the bucket directly.
data "aws_iam_policy_document" "backup_bucket" {
  statement {
    effect    = "Allow"
    actions   = ["s3:ListBucket", "s3:GetBucketLocation"]
    resources = ["arn:aws:s3:::${var.backup_bucket_name}"]
  }

  statement {
    effect = "Allow"
    actions = [
      "s3:PutObject",
      "s3:AbortMultipartUpload",
      "s3:GetObject",
      "s3:DeleteObject"
    ]
    resources = ["arn:aws:s3:::${var.backup_bucket_name}/*"]
  }
}

resource "aws_iam_policy" "backup" {
  name   = "${local.name}-backup-policy"
  policy = data.aws_iam_policy_document.backup_bucket.json
}

resource "aws_iam_role_policy_attachment" "backup" {
  role       = aws_iam_role.backup.name
  policy_arn = aws_iam_policy.backup.arn
}

resource "aws_iam_instance_profile" "backup" {
  name = "${local.name}-instance-profile"
  role = aws_iam_role.backup.name
}
