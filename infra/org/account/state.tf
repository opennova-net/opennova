# ---------------------------------------------------------------------------
# The account's Terraform state bucket
# ---------------------------------------------------------------------------
#
# opennova-tfstate-<account id> (the account id keeps the global name free):
# infra/aws's state (infra/aws/terraform.tfstate), and in prod's account the
# shared stacks' (infra/zone, infra/github) and the org stack's own
# (org/terraform.tfstate), each with the S3 backend's lock file beside it
# (use_lockfile: no DynamoDB table). Versioned (every state ever written stays
# recoverable), encrypted, private, TLS only, never destroyed by a plan, and
# reached by the deploy user, the account's administrator role and its root
# alone.

locals {
  deploy_user_name = "opennova-deploy"
  deploy_user_arn  = "arn:aws:iam::${var.account_id}:user/${local.deploy_user_name}"
  admin_role_arn   = "arn:aws:iam::${var.account_id}:role/OrganizationAccountAccessRole"
}

resource "aws_s3_bucket" "state" {
  bucket        = "opennova-tfstate-${var.account_id}"
  force_destroy = false

  lifecycle {
    prevent_destroy = true
  }
}

resource "aws_s3_bucket_versioning" "state" {
  bucket = aws_s3_bucket.state.id

  versioning_configuration {
    status = "Enabled"
  }
}

resource "aws_s3_bucket_server_side_encryption_configuration" "state" {
  bucket = aws_s3_bucket.state.id

  rule {
    apply_server_side_encryption_by_default {
      sse_algorithm = "AES256"
    }
  }
}

resource "aws_s3_bucket_public_access_block" "state" {
  bucket = aws_s3_bucket.state.id

  block_public_acls       = true
  block_public_policy     = true
  ignore_public_acls      = true
  restrict_public_buckets = true
}

resource "aws_s3_bucket_policy" "state" {
  bucket = aws_s3_bucket.state.id
  policy = jsonencode({
    Version = "2012-10-17"
    Statement = [
      {
        Sid       = "TlsOnly"
        Effect    = "Deny"
        Principal = "*"
        Action    = "s3:*"
        Resource  = [aws_s3_bucket.state.arn, "${aws_s3_bucket.state.arn}/*"]
        Condition = { Bool = { "aws:SecureTransport" = "false" } }
      },
      {
        Sid       = "DeployUserAndAdminsAlone"
        Effect    = "Deny"
        Principal = "*"
        Action    = "s3:*"
        Resource  = [aws_s3_bucket.state.arn, "${aws_s3_bucket.state.arn}/*"]
        Condition = {
          ArnNotEquals = {
            "aws:PrincipalArn" = [local.deploy_user_arn, local.admin_role_arn, "arn:aws:iam::${var.account_id}:root"]
          }
        }
      },
    ]
  })

  depends_on = [aws_s3_bucket_public_access_block.state]
}

# The account holds one deployment alone, so its account-wide S3 switch is
# OpenNova's to set: no bucket in it can be made public.
resource "aws_s3_account_public_access_block" "account" {
  block_public_acls       = true
  block_public_policy     = true
  ignore_public_acls      = true
  restrict_public_buckets = true
}
