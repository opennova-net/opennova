#!/bin/sh
# Take a consistent SQLite snapshot and upload it to S3. AWS creds come from
# the EC2 instance role (IMDSv2); BACKUP_BUCKET and DATABASE_PATH from env.
set -eu

: "${BACKUP_BUCKET:?BACKUP_BUCKET is required}"
DB="${DATABASE_PATH:-/data/novaworld.db}"
PREFIX="${BACKUP_PREFIX:-novaworld}"

if [ ! -f "$DB" ]; then
    echo "[backup] no database at $DB yet; nothing to back up" >&2
    exit 0
fi

ts="$(date -u +%Y%m%dT%H%M%SZ)"
tmp="/tmp/${PREFIX}-${ts}.db"

# .backup is the online-consistent snapshot API (safe while the server has
# the DB open). Then VACUUM INTO would also work; .backup is simplest.
sqlite3 "$DB" ".backup '$tmp'"
gzip -f "$tmp"

key="s3://${BACKUP_BUCKET}/${PREFIX}/${PREFIX}-${ts}.db.gz"
aws s3 cp "${tmp}.gz" "$key" --only-show-errors
echo "[backup] uploaded $key ($(wc -c < "${tmp}.gz") bytes)"
rm -f "${tmp}.gz"
