# OpenNova NovaWorld server infrastructure.
#
# Ported from the opennova-int reference deployment with these deltas:
#   - the EC2 key pair is created from a vault-sourced public key
#     (TF_VAR_ssh_public_key) instead of referencing a pre-existing pair
#   - a dedicated unproxied `nw` A record (TTL 300) anchors the launcher's
#     DNS fallback for server-IP resolution
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

  dns_enabled           = var.cloudflare_zone_id != "" && var.cloudflare_domain != ""
  dns_root_name         = "@"
  dns_nw_name           = "nw"
  downloads_dns_enabled = local.dns_enabled

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

# The launcher's resolution anchor: ALWAYS unproxied (game traffic is UDP and
# hosts-file entries need the raw IP), short TTL so cutovers propagate fast.
resource "cloudflare_record" "nw" {
  count   = local.dns_enabled ? 1 : 0
  zone_id = var.cloudflare_zone_id
  name    = local.dns_nw_name
  content = local.public_ip
  type    = "A"
  ttl     = 300
  proxied = false

  comment = "Managed by Terraform - NovaWorld server anchor"
}

# ---------------------------------------------------------------------------
# Downloads bucket + CDN (shared)
# ---------------------------------------------------------------------------

resource "aws_s3_bucket" "downloads" {
  count         = local.manage_shared ? 1 : 0
  bucket        = var.downloads_bucket_name
  force_destroy = false

  tags = merge(var.tags, {
    Name = "${local.name}-downloads"
  })
}

resource "aws_s3_bucket_cors_configuration" "downloads" {
  count  = local.manage_shared ? 1 : 0
  bucket = aws_s3_bucket.downloads[0].id

  cors_rule {
    allowed_methods = ["GET", "HEAD"]
    allowed_origins = ["*"]
    allowed_headers = ["*"]
    max_age_seconds = 300
  }
}

resource "aws_s3_bucket_versioning" "downloads" {
  count  = local.manage_shared ? 1 : 0
  bucket = aws_s3_bucket.downloads[0].id

  versioning_configuration {
    status = "Enabled"
  }
}

resource "aws_s3_bucket_public_access_block" "downloads" {
  count  = local.manage_shared ? 1 : 0
  bucket = aws_s3_bucket.downloads[0].id

  block_public_acls       = false
  block_public_policy     = false
  ignore_public_acls      = false
  restrict_public_buckets = false
}

resource "aws_s3_bucket_policy" "downloads_public_read" {
  count  = local.manage_shared ? 1 : 0
  bucket = aws_s3_bucket.downloads[0].id

  policy = jsonencode({
    Version = "2012-10-17"
    Statement = [
      {
        Sid       = "AllowPublicRead"
        Effect    = "Allow"
        Principal = "*"
        Action    = ["s3:GetObject"]
        Resource  = "${aws_s3_bucket.downloads[0].arn}/*"
      }
    ]
  })
}

resource "aws_s3_account_public_access_block" "this" {
  count = local.manage_shared ? 1 : 0

  block_public_acls       = false
  block_public_policy     = false
  ignore_public_acls      = false
  restrict_public_buckets = false
}

resource "aws_iam_user" "launcher_ci" {
  count = local.manage_shared && var.create_ci_user ? 1 : 0

  name = "${local.name}-launcher-ci"

  tags = merge(var.tags, {
    Purpose = "github-actions-launcher-upload"
  })
}

resource "aws_iam_user_policy" "launcher_ci" {
  count = local.manage_shared && var.create_ci_user ? 1 : 0

  name = "${local.name}-launcher-ci-policy"
  user = aws_iam_user.launcher_ci[0].name

  policy = jsonencode({
    Version = "2012-10-17"
    Statement = [
      {
        Effect   = "Allow"
        Action   = ["s3:ListBucket"]
        Resource = aws_s3_bucket.downloads[0].arn
      },
      {
        Effect = "Allow"
        Action = [
          "s3:GetObject",
          "s3:PutObject",
          "s3:DeleteObject",
          "s3:GetObjectTagging",
          "s3:PutObjectTagging",
          "s3:DeleteObjectTagging"
        ]
        Resource = "${aws_s3_bucket.downloads[0].arn}/*"
      }
    ]
  })
}

resource "aws_iam_access_key" "launcher_ci" {
  count = local.manage_shared && var.create_ci_user ? 1 : 0

  user = aws_iam_user.launcher_ci[0].name
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

# ---------------------------------------------------------------------------
# ACM cert + CloudFront for the downloads domain (shared)
# ---------------------------------------------------------------------------

resource "aws_acm_certificate" "downloads" {
  provider = aws.us_east_1
  count    = local.downloads_dns_enabled ? 1 : 0

  domain_name       = var.downloads_bucket_name
  validation_method = "DNS"

  lifecycle {
    create_before_destroy = true
  }

  tags = merge(var.tags, {
    Name = "${local.name}-downloads-cert"
  })
}

resource "cloudflare_record" "downloads_cert_validation" {
  for_each = local.downloads_dns_enabled ? {
    for dvo in aws_acm_certificate.downloads[0].domain_validation_options : dvo.domain_name => dvo
  } : {}

  zone_id = var.cloudflare_zone_id
  name    = replace(trimsuffix(each.value.resource_record_name, "."), ".${var.cloudflare_domain}", "")
  type    = each.value.resource_record_type
  content = trimsuffix(each.value.resource_record_value, ".")
  ttl     = 300
  proxied = false

  comment = "ACM validation for downloads certificate"
}

resource "aws_acm_certificate_validation" "downloads" {
  provider = aws.us_east_1
  count    = local.downloads_dns_enabled ? 1 : 0

  certificate_arn         = aws_acm_certificate.downloads[0].arn
  validation_record_fqdns = [for record in cloudflare_record.downloads_cert_validation : record.hostname]
}

resource "aws_cloudfront_distribution" "downloads" {
  count = local.downloads_dns_enabled ? 1 : 0

  enabled             = true
  comment             = "Downloads distribution"
  price_class         = "PriceClass_100"
  wait_for_deployment = false

  aliases = [var.downloads_bucket_name]

  # The downloads bucket is public-read, so CloudFront fetches over plain HTTPS
  # from the bucket's regional REST endpoint — a custom HTTP origin, not an S3
  # origin. (s3_origin_config with an empty origin_access_identity does not
  # round-trip through the aws provider and produces a perpetual in-place diff.)
  origin {
    domain_name = aws_s3_bucket.downloads[0].bucket_regional_domain_name
    origin_id   = "downloads-s3-origin"

    custom_origin_config {
      http_port              = 80
      https_port             = 443
      origin_protocol_policy = "https-only"
      origin_ssl_protocols   = ["TLSv1.2"]
    }
  }

  default_cache_behavior {
    target_origin_id       = "downloads-s3-origin"
    viewer_protocol_policy = "redirect-to-https"
    allowed_methods        = ["GET", "HEAD"]
    cached_methods         = ["GET", "HEAD"]
    compress               = true

    forwarded_values {
      query_string = false

      headers = ["Origin"]

      cookies {
        forward = "none"
      }
    }

    min_ttl     = 0
    default_ttl = 3600
    max_ttl     = 86400
  }

  restrictions {
    geo_restriction {
      restriction_type = "none"
    }
  }

  viewer_certificate {
    acm_certificate_arn      = aws_acm_certificate.downloads[0].arn
    ssl_support_method       = "sni-only"
    minimum_protocol_version = "TLSv1.2_2021"
  }

  tags = merge(var.tags, {
    Name = "${local.name}-downloads-cdn"
  })

  depends_on = [aws_acm_certificate_validation.downloads]
}

resource "cloudflare_record" "downloads" {
  count   = local.downloads_dns_enabled ? 1 : 0
  zone_id = var.cloudflare_zone_id
  name    = "downloads"
  content = aws_cloudfront_distribution.downloads[0].domain_name
  type    = "CNAME"
  ttl     = 300
  proxied = false

  comment = "CloudFront distribution for downloads"
}
