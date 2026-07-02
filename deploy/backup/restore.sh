#!/bin/sh
# Restore a backup over the live database. Pass the S3 key (the .db.gz object)
# as $1. Stop the server first so it doesn't hold the file:
#   ./deploy/run.sh app down
#   docker compose ... run --rm backup /backup/restore.sh novaworld/novaworld-<ts>.db.gz
#   ./deploy/run.sh app deploy
set -eu
: "${BACKUP_BUCKET:?BACKUP_BUCKET is required}"
DB="${DATABASE_PATH:-/data/novaworld.db}"

key="${1:?usage: restore.sh <s3-key-under-the-bucket>}"
tmp="/tmp/restore.db.gz"

aws s3 cp "s3://${BACKUP_BUCKET}/${key}" "$tmp" --only-show-errors
gunzip -f "$tmp"

# Keep a safety copy of whatever is there now.
if [ -f "$DB" ]; then
    cp "$DB" "${DB}.pre-restore"
    echo "[restore] saved current db to ${DB}.pre-restore"
fi

mv "/tmp/restore.db" "$DB"
echo "[restore] restored $key -> $DB"
