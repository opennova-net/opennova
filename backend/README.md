# backend/

Persistence layer for the standalone novaworld server.

Contents:
- `migrations/` — SQLite-flavored DDL applied idempotently on server boot via
  `opennova::db::run_migrations` (engine/net/novaworld/db/sqlite.h). Order is
  lexicographic: `0001_*.sql`, `0002_*.sql`, …
- `seed/` — idempotent INSERT statements (`OR IGNORE`) so they can be re-run
  safely. The standalone server applies these after migrations
  during startup; the dev-only `0002_dev_users.sql` is skipped unless
  `SEED_DEV_USERS` is set.
- `data/` — runtime DB files (SQLite). Gitignored. The default location is
  `backend/data/state.db`; override with the `DATABASE_PATH` env var.

Schema is derived from `onnet/alembic/versions/*` (PostgreSQL) collapsed to a
single SQLite-flavored baseline. We don't need history compatibility with the
Postgres deployment — this is a clean SQLite-backed start.

The service database is disposable. A new table or feature goes in a new file; a fix
to an existing table edits the migration that made it, in place. Migrations are
tracked by file name only, so an edited file never runs again on an existing
database: the PR says "the service DB must be reset" in its notes, and its deploy is
`./deploy/run.sh app reset-db` ([DEPLOY.md](../DEPLOY.md), "Resetting the service
database").
