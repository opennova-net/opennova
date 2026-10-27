# ---------------------------------------------------------------------------
# The deployment's downloads host (ADR 0070; DEPLOY.md, "The downloads host"):
# the builds and tools its site's download buttons fetch, prod's at
# downloads.<domain> (the tagged releases), staging's at
# downloads.staging.<domain> (a pull request's debug builds, which talk to
# staging). A private bucket in the deployment's own account, read by a
# CloudFront distribution alone (origin access control), whose certificate ACM
# issues in us-east-1 (CloudFront takes it from there alone), validated by a
# record in the Cloudflare zone; the name is an unproxied CNAME to the
# distribution (Cloudflare's proxy is for the site, not for game archives).
# Nothing without a zone: there is no name to serve it at. The bucket holds the
# products' published releases alone, each product's file under its stable name
# (opennova-game-windows.zip) beside manifest.json (each product's file, size,
# SHA-256 and release), which the data program's downloads.publish job writes
# (its role's downloads_publish, programs.tf; ONNET_DOWNLOADS_URL names the
# bucket to it); the site reads the manifest from any origin (the
# distribution's own CORS response headers).
# ---------------------------------------------------------------------------

locals {
  # prod downloads.<domain>, another deployment downloads.<deployment>.<domain>;
  # empty without a zone.
  downloads_host        = local.dns_enabled ? (local.is_prod ? "downloads.${var.cloudflare_domain}" : "downloads.${local.deployment}.${var.cloudflare_domain}") : ""
  downloads_bucket_name = "${local.name}-downloads-${local.account_id}"

  # CloudFront's managed CachingOptimized cache policy, by its fixed id (the
  # origin's Cache-Control honoured, which the data program sets short on the
  # stable names).
  cloudfront_caching_optimized = "658327ea-f89d-4fab-a63d-7e88639e58f6"
}

resource "aws_s3_bucket" "downloads" {
  count         = local.dns_enabled ? 1 : 0
  bucket        = local.downloads_bucket_name
  force_destroy = false

  tags = merge(var.tags, {
    Name = "${local.name}-downloads"
  })
}

resource "aws_s3_bucket_server_side_encryption_configuration" "downloads" {
  count  = local.dns_enabled ? 1 : 0
  bucket = aws_s3_bucket.downloads[0].id

  rule {
    apply_server_side_encryption_by_default {
      sse_algorithm = "AES256"
    }
  }
}

resource "aws_cloudfront_origin_access_control" "downloads" {
  count                             = local.dns_enabled ? 1 : 0
  name                              = "${local.name}-downloads"
  description                       = "The ${local.deployment} downloads host's reads of its bucket"
  origin_access_control_origin_type = "s3"
  signing_behavior                  = "always"
  signing_protocol                  = "sigv4"
}

# The certificate, in us-east-1, and its validation record (one name, so one
# record, known at plan by its index).
resource "aws_acm_certificate" "downloads" {
  provider          = aws.us_east_1
  count             = local.dns_enabled ? 1 : 0
  domain_name       = local.downloads_host
  validation_method = "DNS"

  lifecycle {
    create_before_destroy = true
  }

  tags = merge(var.tags, {
    Name = "${local.name}-downloads"
  })
}

resource "cloudflare_record" "downloads_validation" {
  count   = local.dns_enabled ? 1 : 0
  zone_id = var.cloudflare_zone_id
  name    = trimsuffix(tolist(aws_acm_certificate.downloads[0].domain_validation_options)[0].resource_record_name, ".")
  type    = tolist(aws_acm_certificate.downloads[0].domain_validation_options)[0].resource_record_type
  content = trimsuffix(tolist(aws_acm_certificate.downloads[0].domain_validation_options)[0].resource_record_value, ".")
  ttl     = 300
  proxied = false

  comment = "Managed by Terraform - ACM validation of the OpenNova downloads host"
}

resource "aws_acm_certificate_validation" "downloads" {
  provider                = aws.us_east_1
  count                   = local.dns_enabled ? 1 : 0
  certificate_arn         = aws_acm_certificate.downloads[0].arn
  validation_record_fqdns = [cloudflare_record.downloads_validation[0].hostname]
}

# Access-Control-Allow-Origin: * on every answer, whatever the request's headers. The managed
# CORS policies allow no request header beyond the safelisted ones, and CloudFront then leaves
# the header off: a browser's fetch carries Priority (and Cache-Control on a revalidation), so
# the site could never read the manifest.
resource "aws_cloudfront_response_headers_policy" "downloads" {
  count   = local.dns_enabled ? 1 : 0
  name    = "${local.name}-downloads-cors"
  comment = "The ${local.deployment} downloads host: any origin reads it"

  cors_config {
    access_control_allow_credentials = false
    origin_override                  = true

    access_control_allow_headers {
      items = ["*"]
    }
    access_control_allow_methods {
      items = ["GET", "HEAD"]
    }
    access_control_allow_origins {
      items = ["*"]
    }
  }
}

resource "aws_cloudfront_distribution" "downloads" {
  count               = local.dns_enabled ? 1 : 0
  enabled             = true
  comment             = "The ${local.deployment} downloads host"
  aliases             = [local.downloads_host]
  price_class         = "PriceClass_100"
  is_ipv6_enabled     = true
  http_version        = "http2and3"
  wait_for_deployment = false

  origin {
    origin_id                = "downloads"
    domain_name              = aws_s3_bucket.downloads[0].bucket_regional_domain_name
    origin_access_control_id = aws_cloudfront_origin_access_control.downloads[0].id
  }

  default_cache_behavior {
    target_origin_id           = "downloads"
    viewer_protocol_policy     = "redirect-to-https"
    allowed_methods            = ["GET", "HEAD", "OPTIONS"]
    cached_methods             = ["GET", "HEAD"]
    compress                   = true
    cache_policy_id            = local.cloudfront_caching_optimized
    response_headers_policy_id = aws_cloudfront_response_headers_policy.downloads[0].id
  }

  restrictions {
    geo_restriction {
      restriction_type = "none"
    }
  }

  viewer_certificate {
    acm_certificate_arn      = aws_acm_certificate_validation.downloads[0].certificate_arn
    ssl_support_method       = "sni-only"
    minimum_protocol_version = "TLSv1.2_2021"
  }

  tags = merge(var.tags, {
    Name = "${local.name}-downloads"
  })
}

# The distribution reads the bucket, and nothing else does (no public grant:
# the account's S3 public access block stands).
resource "aws_s3_bucket_policy" "downloads" {
  count  = local.dns_enabled ? 1 : 0
  bucket = aws_s3_bucket.downloads[0].id
  policy = jsonencode({
    Version = "2012-10-17"
    Statement = [{
      Sid       = "DownloadsDistributionReads"
      Effect    = "Allow"
      Principal = { Service = "cloudfront.amazonaws.com" }
      Action    = "s3:GetObject"
      Resource  = "arn:aws:s3:::${local.downloads_bucket_name}/*"
      Condition = { StringEquals = { "AWS:SourceArn" = aws_cloudfront_distribution.downloads[0].arn } }
    }]
  })
}

resource "cloudflare_record" "downloads" {
  count   = local.dns_enabled ? 1 : 0
  zone_id = var.cloudflare_zone_id
  name    = local.is_prod ? "downloads" : "downloads.${local.deployment}"
  content = aws_cloudfront_distribution.downloads[0].domain_name
  type    = "CNAME"
  ttl     = 300
  proxied = false

  comment = "Managed by Terraform - OpenNova downloads host (CloudFront, never proxied)"
}
