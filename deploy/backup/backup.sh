#!/usr/bin/env bash
# Dump the service database (pg_dump, custom format: compressed, and
# pg_restore picks what to restore from it) and upload the dump to S3. AWS
# creds come from the EC2 instance role (IMDSv2); BACKUP_BUCKET,
# DATABASE_URL and DATABASE_PASSWORD from env.
set -euo pipefail

: "${BACKUP_BUCKET:?BACKUP_BUCKET is required}"
: "${DATABASE_URL:?DATABASE_URL is required}"
PREFIX="${BACKUP_PREFIX:-novaworld}"
# The pg tools read the password from PGPASSWORD; the URI may also name one.
if [ -n "${DATABASE_PASSWORD:-}" ]; then
    export PGPASSWORD="$DATABASE_PASSWORD"
fi

ts="$(date -u +%Y%m%dT%H%M%SZ)"
tmp="/tmp/${PREFIX}-${ts}.dump"
trap 'rm -f "$tmp"' EXIT

# pg_dump reads one snapshot of the whole database while the server keeps
# writing. No owners or grants: the dump restores under whichever role runs
# pg_restore, into this instance or a new one.
pg_dump --format=custom --no-owner --no-privileges --dbname="$DATABASE_URL" --file="$tmp"

key="s3://${BACKUP_BUCKET}/${PREFIX}/${PREFIX}-${ts}.dump"
aws s3 cp "$tmp" "$key" --only-show-errors
echo "[backup] uploaded $key ($(wc -c < "$tmp") bytes)"
