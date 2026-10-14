# OpenNova NovaWorld server infrastructure.
#
# Ported from the opennova-int reference deployment with these deltas:
#   - the EC2 key pair is created from a vault-sourced public key
#     (TF_VAR_ssh_public_key) instead of referencing a pre-existing pair
#   - IMDSv2 is required on the instance
#   - the backups bucket gets a lifecycle policy (expire after 90 days)
#   - user_data installs docker + the compose plugin ONLY (the target host
#     needs no other dependency; deploys drive it over DOCKER_HOST=ssh://)

terraform {
  required_version = ">= 1.6.0"

  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "~> 5.0"
    }
    cloudflare = {
      source  = "cloudflare/cloudflare"
      version = "~> 4.0"
    }
  }
}

provider "aws" {
  region = var.region
}

# Kept only so the apply that retires the downloads certificate (it lived in
# us-east-1 for CloudFront) can destroy it; drop it after that apply.
provider "aws" {
  alias  = "us_east_1"
  region = "us-east-1"
}

provider "cloudflare" {
  api_token = var.cloudflare_api_token
}

data "aws_availability_zones" "available" {
  state = "available"
}

locals {
  name = var.instance_name

  # Single environment: the account-singleton resources are always managed.
  manage_shared = true

  allocate_eip = var.allocate_eip

  dns_enabled   = var.cloudflare_zone_id != "" && var.cloudflare_domain != ""
  dns_root_name = "@"

  public_ip = local.allocate_eip ? aws_eip.server[0].public_ip : aws_instance.server.public_ip
}

resource "aws_vpc" "auto" {
  count = var.vpc_id == "" ? 1 : 0

  cidr_block           = var.auto_vpc_cidr
  enable_dns_support   = true
  enable_dns_hostnames = true

  tags = merge(var.tags, {
    Name = "${local.name}-vpc"
  })
}

resource "aws_internet_gateway" "auto" {
  count = var.vpc_id == "" ? 1 : 0

  vpc_id = aws_vpc.auto[0].id

  tags = merge(var.tags, {
    Name = "${local.name}-igw"
  })
}

resource "aws_subnet" "auto" {
  count = var.vpc_id == "" && var.subnet_id == "" ? 1 : 0

  vpc_id                  = aws_vpc.auto[0].id
  cidr_block              = var.auto_subnet_cidr
  availability_zone       = data.aws_availability_zones.available.names[0]
  map_public_ip_on_launch = true

  tags = merge(var.tags, {
    Name = "${local.name}-subnet"
  })
}

resource "aws_route_table" "auto" {
  count = var.vpc_id == "" ? 1 : 0

  vpc_id = aws_vpc.auto[0].id

  route {
    cidr_block = "0.0.0.0/0"
    gateway_id = aws_internet_gateway.auto[0].id
  }

  tags = merge(var.tags, {
    Name = "${local.name}-rt"
  })
}

resource "aws_route_table_association" "auto" {
  count = var.vpc_id == "" && var.subnet_id == "" ? 1 : 0

  subnet_id      = aws_subnet.auto[0].id
  route_table_id = aws_route_table.auto[0].id
}

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
  target_vpc_id = var.vpc_id != "" ? var.vpc_id : aws_vpc.auto[0].id
  target_subnet = var.subnet_id != "" ? var.subnet_id : aws_subnet.auto[0].id
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
  vpc_id      = local.target_vpc_id

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
  subnet_id                   = local.target_subnet
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
  count    = local.allocate_eip ? 1 : 0
  instance = aws_instance.server.id

  tags = merge(var.tags, {
    Name = "${local.name}-eip"
  })
}

# ---------------------------------------------------------------------------
# Cloudflare DNS
# ---------------------------------------------------------------------------

# Web root (example.com / www). Proxied by default so the site gets Cloudflare TLS.
resource "cloudflare_record" "web_root" {
  count   = local.dns_enabled ? 1 : 0
  zone_id = var.cloudflare_zone_id
  name    = local.dns_root_name
  content = local.public_ip
  type    = "A"
  ttl     = var.cloudflare_web_proxied ? 1 : 300
  proxied = var.cloudflare_web_proxied

  comment = "Managed by Terraform - OpenNova web"
}

resource "cloudflare_record" "web_www" {
  count   = local.dns_enabled ? 1 : 0
  zone_id = var.cloudflare_zone_id
  name    = "www"
  content = local.public_ip
  type    = "A"
  ttl     = var.cloudflare_web_proxied ? 1 : 300
  proxied = var.cloudflare_web_proxied

  comment = "Managed by Terraform - OpenNova web (www)"
}

# The web build (ADR 0049): the portal's nginx routes game.<domain> to the game
# container. Proxied like the root, so it gets the same Cloudflare TLS.
resource "cloudflare_record" "game" {
  count   = local.dns_enabled ? 1 : 0
  zone_id = var.cloudflare_zone_id
  name    = "game"
  content = local.public_ip
  type    = "A"
  ttl     = var.cloudflare_web_proxied ? 1 : 300
  proxied = var.cloudflare_web_proxied

  comment = "Managed by Terraform - OpenNova web build"
}

# Edge authentication (DEPLOY.md), created only once var.edge_auth_secret is
# set and the web records are proxied: a request-header Transform Rule stamps
# X-OpenNova-Edge: <secret> on every request Cloudflare forwards for the web
# hostnames, so the portal's nginx (EDGE_AUTH_SECRET) can refuse /api/
# requests that reached it any other way, a Cloudflare Worker on someone else's
# zone fetching this origin with a CF-Connecting-IP of its choosing included.
# Unproxied records reach the origin with no Transform Rule, so the rule (and
# the secret beside it) would only refuse every /api/ request. A zone holds one
# entrypoint ruleset per phase: import an existing late-transform ruleset
# before applying, and Terraform then owns all its rules (this one replaces
# them).
resource "cloudflare_ruleset" "edge_auth" {
  count       = local.dns_enabled && var.cloudflare_web_proxied && var.edge_auth_secret != "" ? 1 : 0
  zone_id     = var.cloudflare_zone_id
  name        = "opennova-edge-auth"
  description = "Stamps the edge-authentication header the portal's nginx checks"
  kind        = "zone"
  phase       = "http_request_late_transform"

  rules {
    action      = "rewrite"
    description = "X-OpenNova-Edge on the web hostnames"
    enabled     = true
    expression  = "(http.host in {\"${var.cloudflare_domain}\" \"www.${var.cloudflare_domain}\"})"

    action_parameters {
      headers {
        name      = "X-OpenNova-Edge"
        operation = "set"
        value     = var.edge_auth_secret
      }
    }
  }
}

# A secret with unproxied web records makes no rule (above); the plan says so,
# since the web container given the same secret would then refuse every /api/.
check "edge_auth_needs_proxied_web_records" {
  assert {
    condition     = var.edge_auth_secret == "" || var.cloudflare_web_proxied
    error_message = "edge_auth_secret is set but cloudflare_web_proxied is false: no Transform Rule is made, so keep EDGE_AUTH_SECRET out of the web container's environment (DEPLOY.md, Edge authentication)."
  }
}

# The account-wide S3 public access block: every bucket here is private.
resource "aws_s3_account_public_access_block" "this" {
  count = local.manage_shared ? 1 : 0

  block_public_acls       = true
  block_public_policy     = true
  ignore_public_acls      = true
  restrict_public_buckets = true
}

# ---------------------------------------------------------------------------
# Backups bucket (shared) + instance role
# ---------------------------------------------------------------------------

resource "aws_s3_bucket" "backups" {
  count         = local.manage_shared ? 1 : 0
  bucket        = var.backup_bucket_name
  force_destroy = false

  tags = merge(var.tags, {
    Name = "${local.name}-db-backups"
  })
}

resource "aws_s3_bucket_versioning" "backups" {
  count  = local.manage_shared ? 1 : 0
  bucket = aws_s3_bucket.backups[0].id

  versioning_configuration {
    status = "Enabled"
  }
}

resource "aws_s3_bucket_server_side_encryption_configuration" "backups" {
  count  = local.manage_shared ? 1 : 0
  bucket = aws_s3_bucket.backups[0].id

  rule {
    apply_server_side_encryption_by_default {
      sse_algorithm = "AES256"
    }
  }
}

resource "aws_s3_bucket_lifecycle_configuration" "backups" {
  count  = local.manage_shared ? 1 : 0
  bucket = aws_s3_bucket.backups[0].id

  rule {
    id     = "expire-old-backups"
    status = "Enabled"

    filter {} # whole bucket

    expiration {
      days = 90
    }

    noncurrent_version_expiration {
      noncurrent_days = 30
    }
  }
}

resource "aws_s3_bucket_public_access_block" "backups" {
  count  = local.manage_shared ? 1 : 0
  bucket = aws_s3_bucket.backups[0].id

  block_public_acls       = true
  block_public_policy     = true
  ignore_public_acls      = true
  restrict_public_buckets = true
}

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
