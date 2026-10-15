# ---------------------------------------------------------------------------
# The operations bucket (backups) and the account's S3 public access block,
# both account-wide: prod (the default workspace) owns them, another deployment
# uses them by name.
# ---------------------------------------------------------------------------

# The account-wide S3 public access block: every bucket here is private.
resource "aws_s3_account_public_access_block" "this" {
  count = local.manage_shared ? 1 : 0

  block_public_acls       = true
  block_public_policy     = true
  ignore_public_acls      = true
  restrict_public_buckets = true
}

resource "aws_s3_bucket" "ops" {
  count         = local.manage_shared ? 1 : 0
  bucket        = var.backup_bucket_name
  force_destroy = false

  tags = merge(var.tags, {
    Name = "${local.name}-db-backups"
  })
}

resource "aws_s3_bucket_versioning" "ops" {
  count  = local.manage_shared ? 1 : 0
  bucket = aws_s3_bucket.ops[0].id

  versioning_configuration {
    status = "Enabled"
  }
}

resource "aws_s3_bucket_server_side_encryption_configuration" "ops" {
  count  = local.manage_shared ? 1 : 0
  bucket = aws_s3_bucket.ops[0].id

  rule {
    apply_server_side_encryption_by_default {
      sse_algorithm = "AES256"
    }
  }
}

resource "aws_s3_bucket_lifecycle_configuration" "ops" {
  count  = local.manage_shared ? 1 : 0
  bucket = aws_s3_bucket.ops[0].id

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

resource "aws_s3_bucket_public_access_block" "ops" {
  count  = local.manage_shared ? 1 : 0
  bucket = aws_s3_bucket.ops[0].id

  block_public_acls       = true
  block_public_policy     = true
  ignore_public_acls      = true
  restrict_public_buckets = true
}
