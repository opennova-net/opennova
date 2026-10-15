# Backup sidecar

A nightly `pg_dump` of the NovaWorld service database to S3, plus on-demand
backup / list / restore. The dumps are copies independent of the database
instance: the managed database keeps its own snapshots and point-in-time
recovery beside them, and a dump restores into a fresh instance as well.

- Runs as a long-lived `backup` service in the prod compose (cron at 03:00 UTC).
- `postgres:17-alpine`: the client tools match the server's major version.
- AWS credentials come from the EC2 instance IAM role over IMDSv2; no keys in
  the container.
- The database comes from `DATABASE_URL` (a libpq URI) and `DATABASE_PASSWORD`,
  the server's own settings. The image carries the pinned RDS CA bundle at
  `/app/rds-global-bundle.pem`, where the server image has it, so a URI with
  `sslmode=verify-full&sslrootcert=/app/rds-global-bundle.pem` works in both.
- `BACKUP_BUCKET` is the backups S3 bucket from `terraform output
  backup_bucket_name`; the toolbox sets it automatically. `BACKUP_PREFIX`
  (default `novaworld`) is the key prefix.

A dump is custom format (`pg_dump -Fc`: compressed; `pg_restore` reads it)
with no owners or grants, so it restores under whichever role runs the
restore.

## Commands (via the deploy toolbox)

```bash
./deploy/run.sh backup now      # dump -> s3://<bucket>/novaworld/novaworld-<ts>.dump
./deploy/run.sh backup list     # list dumps, newest last
```

## Restore (server must be down)

```bash
./deploy/run.sh app down
# from the toolbox shell:
docker compose -f deploy/compose/docker-compose.yml \
               -f deploy/compose/docker-compose.prod.yml \
  run --rm backup /backup/restore.sh novaworld/novaworld-<ts>.dump
./deploy/run.sh app deploy
```

`restore.sh` first dumps the database as it stands to
`novaworld/novaworld-pre-restore-<ts>.dump`, then restores in one transaction
(`pg_restore --clean --if-exists --single-transaction`): every table in the
dump is dropped and recreated, or nothing changes. The server's next boot
applies any migration the dump predates. The backups bucket has a 90-day
lifecycle (terraform).
