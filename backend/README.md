# backend/

Persistence layer for the standalone novaworld server.

Contents:
- `migrations/` — SQLite-flavored DDL applied idempotently on server boot via
  `opennova::db::run_migrations` (libs/novaworld/db/sqlite.h). Order is
  lexicographic: `0001_*.sql`, `0002_*.sql`, …
- `seed/` — INSERT statements with `OR IGNORE` so they can be re-run safely.
  The standalone server applies these after migrations during startup.
- `data/` — runtime DB files (SQLite). Gitignored. The default location is
  `backend/data/state.db`; override with the `DATABASE_PATH` env var.

Schema is derived from `onnet/alembic/versions/*` (PostgreSQL) collapsed to a
single SQLite-flavored baseline. We don't need history compatibility with the
Postgres deployment — this is a clean SQLite-backed start. New schema
changes go in new files, not by editing `0001_initial.sql`.
