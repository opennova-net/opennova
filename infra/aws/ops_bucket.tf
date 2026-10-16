# ---------------------------------------------------------------------------
# The deployment's operations bucket, in its own account: the release
# manifests (releases/<id>.json, which `app deploy` and `app rollback` write
# and read), the hosted game servers' game data (data/: the data sets' trees,
# ADR 0064; fleet.tf and programs.tf scope the roles to it) and the database
# dumps (novaworld/, the backup task's). Named after
# the deployment and its account (opennova-prod-ops-<account id>), so no other
# deployment names it; one bucket per deployment, so the game data's prefix
# carries no deployment segment. The account's S3 public access block is
# infra/org's.
# ---------------------------------------------------------------------------

locals {
  ops_bucket_name = "${local.name}-ops-${local.account_id}"
}

resource "aws_s3_bucket" "ops" {
  bucket        = local.ops_bucket_name
  force_destroy = false

  tags = merge(var.tags, {
    Name = "${local.name}-ops"
  })
}

resource "aws_s3_bucket_versioning" "ops" {
  bucket = aws_s3_bucket.ops.id

  versioning_configuration {
    status = "Enabled"
  }
}

resource "aws_s3_bucket_server_side_encryption_configuration" "ops" {
  bucket = aws_s3_bucket.ops.id

  rule {
    apply_server_side_encryption_by_default {
      sse_algorithm = "AES256"
    }
  }
}

resource "aws_s3_bucket_lifecycle_configuration" "ops" {
  bucket = aws_s3_bucket.ops.id

  # The database dumps (novaworld/): 90 days.
  rule {
    id     = "expire-old-backups"
    status = "Enabled"

    filter {
      prefix = "${local.backup_prefix}/"
    }

    expiration {
      days = 90
    }

    noncurrent_version_expiration {
      noncurrent_days = 30
    }
  }

  # The release manifests (releases/, a few KiB each) never expire: `app
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

  # The game data (data/): an upload the admin page abandoned mid-way (its
  # parts are in the bucket until it completes or is aborted) is aborted after
  # 7 days, its parts with it; the worker aborts the ones it knows of sooner.
  rule {
    id     = "data-abandoned-uploads"
    status = "Enabled"

    filter {
      prefix = "data/"
    }

    abort_incomplete_multipart_upload {
      days_after_initiation = 7
    }
  }

  # The trees (immutable: a pruned tree's objects are deleted, and the
  # versions that leaves are kept 7 days).
  rule {
    id     = "data-trees-old-versions"
    status = "Enabled"

    filter {
      prefix = "data/trees/"
    }

    noncurrent_version_expiration {
      noncurrent_days = 7
    }
  }
}

resource "aws_s3_bucket_public_access_block" "ops" {
  bucket = aws_s3_bucket.ops.id

  block_public_acls       = true
  block_public_policy     = true
  ignore_public_acls      = true
  restrict_public_buckets = true
}
