# Backup task

A nightly `pg_dump` of the NovaWorld service database to S3, plus on-demand
backup / list / restore. The dumps are copies independent of the database
instance: the managed database keeps its own snapshots and point-in-time
recovery beside them, and a dump restores into a fresh instance as well.

- Published as `<registry>/opennova-novaworld-backup` beside the programs' images, and run
  as an ECS task (`infra/aws/backup.tf`): EventBridge Scheduler starts `/backup/backup.sh`
  nightly at 03:00 UTC, and the deploy toolbox runs the same task definition for its
  one-off work (`backup now|restore`, the `db` commands), a command each. A deploy pins the
  image by digest like the programs'.
- `postgres:17.11-alpine3.24`, pinned by digest (the Dockerfile says how to bump it): the
  client tools match the server's major version.
- AWS credentials come from the task's role (`opennova-<deployment>-backup`: the deployment's
  dumps alone, and the master password the `db` commands lend it); no keys in the
  container.
- The database comes from `DATABASE_URL` (a libpq URI) and `DATABASE_PASSWORD`,
  the server's own settings. The image carries the pinned RDS CA bundle at
  `/app/rds-global-bundle.pem`, where the server image has it, so a URI with
  `sslmode=verify-full&sslrootcert=/app/rds-global-bundle.pem` works in both.
- `BACKUP_BUCKET` is the deployment's operations bucket and `BACKUP_PREFIX` the dumps'
  prefix (`novaworld`; one bucket per deployment, so it names none), both in the task
  definition.

A dump is custom format (`pg_dump -Fc`: compressed; `pg_restore` reads it)
with no owners or grants, so it restores under whichever role runs the
restore.

## Commands (via the deploy toolbox)

Each names its deployment (`backup list` alone reads prod without one):

```bash
./deploy/run.sh --deployment staging backup now    # dump -> s3://<bucket>/novaworld/novaworld-<ts>.dump
./deploy/run.sh --deployment staging backup list   # list dumps, newest last
```

## Restore

```bash
./deploy/run.sh --deployment staging backup restore novaworld/novaworld-<ts>.dump --yes-overwrite-database
```

It stops every program (`app down`), runs `restore.sh` in a one-off task and starts them
again (`app up`). `restore.sh` first dumps the database as it stands to
`novaworld/novaworld-pre-restore-<ts>.dump`, then restores in one transaction
(`pg_restore --clean --if-exists --single-transaction`): every table in the
dump is dropped and recreated, or nothing changes. The server's next boot
applies any migration the dump predates. The operations bucket keeps the dumps 90 days
(`infra/aws/ops_bucket.tf`). DEPLOY.md, section 6, has the rest: the restore drill, `backup
import`.
