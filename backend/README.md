# backend/

Persistence layer for the standalone novaworld server.

Contents:
- `migrations/` — SQLite-flavored DDL applied idempotently on server boot via
  `opennova::db::run_migrations` (engine/net/novaworld/db/sqlite.h). Order is
  lexicographic: `0001_*.sql`, `0002_*.sql`, …
- `seed/` — idempotent INSERT statements (`OR IGNORE`, or `ON CONFLICT ... DO
  UPDATE` in the Terraform-generated `0002_expansions.generated.sql`) so they
  can be re-run safely. The standalone server applies these after migrations
  during startup; the dev-only `0002_dev_users.sql` is skipped unless
  `SEED_DEV_USERS` is set.
- `data/` — runtime DB files (SQLite). Gitignored. The default location is
  `backend/data/state.db`; override with the `DATABASE_PATH` env var.

Schema is derived from `onnet/alembic/versions/*` (PostgreSQL) collapsed to a
single SQLite-flavored baseline. We don't need history compatibility with the
Postgres deployment — this is a clean SQLite-backed start. New schema
changes go in new files, not by editing `0001_initial.sql`.
