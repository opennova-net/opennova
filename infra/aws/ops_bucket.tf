# ---------------------------------------------------------------------------
# The operations bucket, account-wide: prod (the default workspace) owns it, another deployment
# uses them by name. The bucket holds, per deployment, the release bundles
# (releases/<deployment>/<id>.tar.gz and its manifest <id>.json, which `app
# deploy` uploads and the host's converge downloads) and the database dumps
# (the backup sidecar's: novaworld/ for prod, novaworld-<deployment>/
# otherwise). Its name is the one the single-host stack's backups bucket had.
# ---------------------------------------------------------------------------

# The account-wide S3 public access block is not OpenNova's: the account holds
# other projects, and an account-wide switch here would undo theirs. Forgotten,
# never destroyed (the setting stays as it is); the ops bucket's own block keeps
# it private.
removed {
  from = aws_s3_account_public_access_block.this

  lifecycle {
    destroy = false
  }
}

resource "aws_s3_bucket" "ops" {
  count         = local.manage_shared ? 1 : 0
  bucket        = var.ops_bucket_name
  force_destroy = false

  tags = merge(var.tags, {
    Name = "${local.name}-ops"
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

  # The database dumps, every deployment's (novaworld/, novaworld-<deployment>/;
  # the single-host stack's SQLite backups under novaworld/ among them): 90 days.
  rule {
    id     = "expire-old-backups"
    status = "Enabled"

    filter {
      prefix = "novaworld"
    }

    expiration {
      days = 90
    }

    noncurrent_version_expiration {
      noncurrent_days = 30
    }
  }

  # The release bundles (releases/<deployment>/, a few KiB each) never expire:
  # a new host converges to the current one however old it is, and `app
  # rollback --to` reaches any.
  rule {
    id     = "release-old-versions"
    status = "Enabled"

    filter {
      prefix = "releases/"
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
