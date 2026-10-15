#!/usr/bin/env bash
# Restore a dump over the service database. Pass the S3 key (the .dump object)
# as $1. Stop the server first, so nothing writes while the tables are
# replaced:
#   ./deploy/run.sh app down
#   docker compose ... run --rm backup /backup/restore.sh novaworld/novaworld-<ts>.dump
#   ./deploy/run.sh app deploy
#
# The database as it stands is dumped to S3 first (novaworld-pre-restore-<ts>
# under the same prefix). The restore is one transaction: every object in the
# dump is dropped and recreated (--clean --if-exists), or nothing changes.
set -euo pipefail
: "${BACKUP_BUCKET:?BACKUP_BUCKET is required}"
: "${DATABASE_URL:?DATABASE_URL is required}"
PREFIX="${BACKUP_PREFIX:-novaworld}"
if [ -n "${DATABASE_PASSWORD:-}" ]; then
    export PGPASSWORD="$DATABASE_PASSWORD"
fi

key="${1:?usage: restore.sh <s3-key-under-the-bucket>}"
ts="$(date -u +%Y%m%dT%H%M%SZ)"
dump="/tmp/restore.dump"
safety="/tmp/${PREFIX}-pre-restore-${ts}.dump"
trap 'rm -f "$dump" "$safety"' EXIT

aws s3 cp "s3://${BACKUP_BUCKET}/${key}" "$dump" --only-show-errors

# Keep a safety copy of whatever is there now.
pg_dump --format=custom --no-owner --no-privileges --dbname="$DATABASE_URL" --file="$safety"
aws s3 cp "$safety" "s3://${BACKUP_BUCKET}/${PREFIX}/$(basename "$safety")" --only-show-errors
echo "[restore] saved the current database to s3://${BACKUP_BUCKET}/${PREFIX}/$(basename "$safety")"

pg_restore --clean --if-exists --no-owner --no-privileges --single-transaction \
    --dbname="$DATABASE_URL" "$dump"
echo "[restore] restored $key"
