#!/usr/bin/env bash
# List the database dumps in S3, newest last.
set -euo pipefail
: "${BACKUP_BUCKET:?BACKUP_BUCKET is required}"
PREFIX="${BACKUP_PREFIX:-novaworld}"
aws s3 ls "s3://${BACKUP_BUCKET}/${PREFIX}/" --recursive --human-readable | sort
