# Backup sidecar

Nightly SQLite snapshot to S3, plus on-demand backup / list / restore.

- Runs as a long-lived `backup` service in the prod compose (cron at 03:00 UTC).
- AWS credentials come from the EC2 instance IAM role over IMDSv2; no keys in
  the container.
- `BACKUP_BUCKET` is the backups S3 bucket from `terraform output
  backup_bucket_name`; the toolbox sets it automatically.

## Commands (via the deploy toolbox)

```bash
./deploy/run.sh backup now      # snapshot -> s3://<bucket>/novaworld/<ts>.db.gz
./deploy/run.sh backup list     # list backups, newest last
```

## Restore (server must be down)

```bash
./deploy/run.sh app down
# from the toolbox shell, against the remote engine:
docker compose -f deploy/compose/docker-compose.yml \
               -f deploy/compose/docker-compose.prod.yml \
  run --rm backup /backup/restore.sh novaworld/novaworld-<ts>.db.gz
./deploy/run.sh app deploy
```

`restore.sh` keeps a `${DATABASE_PATH}.pre-restore` safety copy before
overwriting. The backups bucket has a 90-day lifecycle (terraform).
