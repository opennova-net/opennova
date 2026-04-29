-- Initial schema for the standalone novaworld server (SQLite flavor).
--
-- Consolidates onnet/onnw/alembic/versions/001..014 into a single squashed
-- baseline. We don't need history compatibility with the Postgres deployment
-- — this is a clean SQLite-backed start. New schema changes go in
-- 0002_*.sql, 0003_*.sql, etc.
--
-- Translation notes from the source Alembic:
--   PostgreSQL SERIAL          -> SQLite INTEGER PRIMARY KEY
--   PostgreSQL TIMESTAMPTZ     -> SQLite TEXT (ISO 8601 / CURRENT_TIMESTAMP)
--   PostgreSQL JSON            -> SQLite TEXT (use json_*() funcs if needed)
--   PostgreSQL BIGINT          -> SQLite INTEGER (already 64-bit)
--   PostgreSQL BOOLEAN         -> SQLite INTEGER (0/1)
--   PostgreSQL NOW()           -> SQLite CURRENT_TIMESTAMP

-- Players (auth + handle).
-- From 001_create_players_table + 002_add_player_handles.
CREATE TABLE players (
    id            INTEGER PRIMARY KEY,
    username      TEXT NOT NULL UNIQUE,
    password_hash TEXT NOT NULL,
    pcid          TEXT NOT NULL UNIQUE,
    nwh           TEXT NOT NULL,
    nwhandle      TEXT NOT NULL,
    created_at    TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_login    TEXT
);
CREATE INDEX idx_players_username ON players(username);
CREATE INDEX idx_players_pcid     ON players(pcid);

-- Games (Joint Operations / DFX2 / etc.).
-- From 005_create_games_table + 007 (executable_name).
CREATE TABLE games (
    id              INTEGER PRIMARY KEY,
    slug            TEXT NOT NULL UNIQUE,
    display_name    TEXT NOT NULL,
    lobby_name      TEXT NOT NULL,
    gate_tag        TEXT NOT NULL UNIQUE,
    start_page      TEXT NOT NULL,
    host_page       TEXT NOT NULL,
    gsb_path        TEXT NOT NULL,
    join_lan_url    TEXT,
    ver1            TEXT NOT NULL,
    ver2            TEXT NOT NULL,
    exp_bits        TEXT NOT NULL,
    pfid            TEXT,
    executable_name TEXT NOT NULL,
    created_at      TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at      TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

-- Hosts (live game-server entries broadcast in the lobby browser).
-- From 003_create_hosts_table, 005 (game_id), 006 (drop app_data).
CREATE TABLE hosts (
    id           INTEGER PRIMARY KEY,
    rid          INTEGER NOT NULL,
    gsid         TEXT NOT NULL,
    app_id       TEXT,
    ip_address   TEXT,
    port         INTEGER,
    server_name  TEXT,
    pcid_key     TEXT,
    host_key     TEXT,
    player_count INTEGER,
    max_players  INTEGER,
    region       TEXT,
    last_seen    TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    created_at   TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    game_id      INTEGER NOT NULL REFERENCES games(id) ON DELETE RESTRICT,
    UNIQUE(rid)
);
CREATE INDEX idx_hosts_last_seen ON hosts(last_seen);

-- Expansions catalogue (Vue /expansions UI).
-- From 007_add_expansions + 014_add_featured_flag_to_expansions.
CREATE TABLE expansions (
    id             INTEGER PRIMARY KEY,
    game_id        INTEGER NOT NULL REFERENCES games(id) ON DELETE CASCADE,
    slug           TEXT NOT NULL UNIQUE,
    display_name   TEXT NOT NULL,
    summary        TEXT,
    version        TEXT NOT NULL,
    package_type   TEXT NOT NULL DEFAULT 'zip',
    install_subdir TEXT NOT NULL,
    featured       INTEGER NOT NULL DEFAULT 0,
    created_at     TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at     TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(game_id, slug)
);

CREATE TABLE expansion_files (
    id           INTEGER PRIMARY KEY,
    expansion_id INTEGER NOT NULL REFERENCES expansions(id) ON DELETE CASCADE,
    download_url TEXT NOT NULL,
    sha256       TEXT NOT NULL,
    size_bytes   INTEGER,
    file_type    TEXT NOT NULL DEFAULT 'archive',
    order_index  INTEGER NOT NULL DEFAULT 1,
    created_at   TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at   TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(expansion_id, file_type, order_index)
);

-- Build/CI integration table (Vue /admin can request a release run).
-- From 008_add_expansion_releases.
CREATE TABLE expansion_releases (
    id            INTEGER PRIMARY KEY,
    slug          TEXT NOT NULL,
    version       TEXT NOT NULL,
    repo_ref      TEXT NOT NULL,
    status        TEXT NOT NULL DEFAULT 'pending',
    notes         TEXT,
    error_message TEXT,
    workflow_url  TEXT,
    target_commit TEXT,
    created_at    TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at    TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    published_at  TEXT,
    UNIQUE(slug, version)
);
