# backend/

Persistence layer for the standalone novaworld server: its PostgreSQL 17
schema and seed data.

Contents:
- `migrations/` — PostgreSQL DDL, applied on server boot by
  `opennova::db::run_migrations` (engine/net/novaworld/db/database.h) in
  lexicographic order: `0001_baseline.sql`, `0002_*.sql`, …. Each file runs in
  its own transaction, and `_schema_migrations` records its name and SHA-256.
  The run holds a database-wide advisory lock, so two servers booting on one
  database apply each file once.
- `seed/` — idempotent inserts (`ON CONFLICT DO NOTHING`), safe to re-run. The
  server applies them after the migrations, under the same lock, at every
  boot; the dev-only `0002_dev_users.sql` is skipped unless `SEED_DEV_USERS` is
  set.

The database is not here: it is the PostgreSQL server's.

`0001_baseline.sql` is the schema the SQLite-era migrations 0001..0015 had
built, squashed into one file in PostgreSQL's dialect: `bigint` integers,
identity ids, `timestamptz` times, `boolean` flags and `bytea` samples (with
the hosted servers' comp grant, `players.comp_servers`, in place of the old
approval flag; `0017_capacity_addresses.sql` moves it to the `comp_grants` table). `0002_billing.sql` adds the billing tables: the service's
instance id, the support tiers' Stripe customers, subscriptions and webhook
events, the cache of Stripe the hosting entitlement reads
(apps/novaworld_common/hosted/README.md). `0005_account_email.sql`
adds the accounts' email addresses (`players.email`, lowercased, one account's)
and the password reset links (apps/novaworld_common/README.md "Account email
and password resets"). `0006_lobby_commands.sql` adds the lobby's tables: the
NovaWorld sessions the lobby holds (`lobby_sessions`) and the commands the
routes and the hosted servers' supervisor address to it (`lobby_commands`),
the only way in and out of the lobby's memory (apps/novaworld_common/README.md
"Connection lifecycle"). `0020_lobby_live_view.sql` adds the account each lobby
session signed in as and the server it plays on, the task each lobby instance
runs as, and keys a listing's joiners (`host_players`) by their account, so its
roster is who plays on it now ("Listed servers and owners"). `0022_lobby_goodbyes.sql`
adds the goodbyes a lobby's clean stop leaves for the sessions it held
(`lobby_goodbyes`), which the lobby their next packets reach sends in its place
("Lobby instances").

## Migrations are append-only

A migration that a database has applied never changes: the server refuses to
boot when a recorded file's SHA-256 no longer matches the file on disk. A new
table, a new column or a fix to an existing one is a new file with the next
number. (The disposable-database practice of the SQLite era, an edited
migration plus a reset, ended with the move to PostgreSQL; the one exception is
the change that brought PostgreSQL in, whose files no deployment had applied
before its cutover, so a database made from an earlier revision of it is
recreated: `scripts/dev_postgres.sh reset`.)

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
