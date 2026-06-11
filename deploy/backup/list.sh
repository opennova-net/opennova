#!/bin/sh
# List the SQLite backups in S3, newest last.
set -eu
: "${BACKUP_BUCKET:?BACKUP_BUCKET is required}"
PREFIX="${BACKUP_PREFIX:-novaworld}"
aws s3 ls "s3://${BACKUP_BUCKET}/${PREFIX}/" --recursive --human-readable | sort
