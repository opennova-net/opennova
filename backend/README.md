# backend/

Persistence layer for the standalone novaworld server: its PostgreSQL 17
schema and seed data.

Contents:
- `migrations/` — PostgreSQL DDL, applied on server boot by
  `opennova::db::run_migrations` (engine/net/novaworld/db/database.h) in
  lexicographic order: `0001_baseline.sql`, the whole schema and the rows it
  starts from, then each change since, one file each (`0002_players_projects.sql`,
  ...). Each file runs in its own transaction, and `_schema_migrations` records
  its name and SHA-256. The run holds a database-wide advisory lock, so two
  servers booting on one database apply each file once.
- `seed/` — idempotent inserts (`ON CONFLICT DO NOTHING`), safe to re-run. The
  server applies them after the migrations, under the same lock, at every
  boot; the dev-only `0002_dev_users.sql` is skipped unless `SEED_DEV_USERS` is
  set.

The database is not here: it is the PostgreSQL server's.

`0001_baseline.sql` is the schema, each table with what it holds, grouped by
what owns it: the accounts, the catalog and the listings; the lobby's state;
the CD keys; billing; the games as products with their releases and projects
(the seeded six in one order, so each keeps its id across a reset); the hosted
servers and their fleet; the jobs and the programs' use. It was squashed at
prod's reset (2026-10) from the 25 migrations that had built it, which git
history keeps.

## Migrations

A schema change is a new migration file, `NNNN_short_name.sql`, numbered after
the last one: ALTER, CREATE, DROP, UPDATE and DELETE statements that take the
schema and the rows any deployment holds from where the files before it left
them, so a release's change upgrades every database in place at its first boot.
A file a deployment has applied is never edited again (0001 included: its header
predates this rule): `_schema_migrations` records each file's SHA-256, and the
server refuses to boot while a recorded file's bytes no longer match, so a fix is
another new file. `tests/novaworld/backend_schema_test.cpp` pins each shipped
file's SHA-256, and checks that a new one sorts after them. A recorded file the
server does not have (a newer release's, or one of the 0002..0025 the squash
replaced) is ignored; a database made before the squash records the old
`0001_baseline.sql` too, so it is refused until reset once (DEPLOY.md,
"Resetting the service database"). A local database is recreated with
`scripts/dev_postgres.sh reset`.

## The database the server needs

The server connects with `DATABASE_URL` (a libpq URI) and `DATABASE_PASSWORD`
(apps/novaworld_common/README.md "Database"). Its database must be created
with the SQL_ASCII encoding and the C locale; `run_migrations` refuses any
other:

```sql
CREATE DATABASE novaworld TEMPLATE template0
    ENCODING 'SQL_ASCII' LC_COLLATE 'C' LC_CTYPE 'C';
```

- **SQL_ASCII** stores text as the bytes it was sent. Retail clients send
  their own code page's bytes (cp1252: server names, rosters, player names),
  which the service keeps byte for byte and hands back to stock clients
  unchanged; a UTF8 database would refuse them. The service converts that text
  to UTF-8 itself where a UTF-8 consumer reads it (the JSON routes).
- **C** orders text byte by byte and folds case for ASCII alone.

`scripts/dev_postgres.sh up` runs a local server whose database is made this
way, and the dev compose stack's `postgres` service is the same
(`POSTGRES_INITDB_ARGS="--locale=C --encoding=SQL_ASCII"`).
