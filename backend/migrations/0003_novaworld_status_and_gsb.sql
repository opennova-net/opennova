-- NovaWorld account/server status and richer GSB host rows.
--
-- This migration is additive so existing development DBs keep their user
-- records while gaining the controls needed to exercise retail failure paths.

ALTER TABLE players ADD COLUMN account_status TEXT NOT NULL DEFAULT 'active';

CREATE TABLE IF NOT EXISTS player_game_access (
    user_id      INTEGER NOT NULL REFERENCES players(id) ON DELETE CASCADE,
    game_slug    TEXT NOT NULL REFERENCES games(slug) ON DELETE CASCADE,
    status       TEXT NOT NULL DEFAULT 'active',
    exp_bits     TEXT NOT NULL,
    created_at   TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at   TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (user_id, game_slug)
);

CREATE TABLE IF NOT EXISTS server_status (
    id                  INTEGER PRIMARY KEY CHECK (id = 1),
    maintenance_enabled INTEGER NOT NULL DEFAULT 0,
    message             TEXT NOT NULL DEFAULT 'NovaWorld is temporarily unavailable.',
    updated_at          TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

INSERT OR IGNORE INTO server_status (id, maintenance_enabled, message)
VALUES (1, 0, 'NovaWorld is temporarily unavailable.');

CREATE TABLE IF NOT EXISTS active_user_sessions (
    user_id       INTEGER PRIMARY KEY REFERENCES players(id) ON DELETE CASCADE,
    username      TEXT NOT NULL,
    session_tag   TEXT NOT NULL,
    persistent_id TEXT,
    remote_ip     TEXT,
    user_agent    TEXT,
    created_at    TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_seen_at  TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_active_user_sessions_tag
    ON active_user_sessions(session_tag);

CREATE INDEX IF NOT EXISTS idx_active_user_sessions_persistent
    ON active_user_sessions(persistent_id);

ALTER TABLE active_hosts ADD COLUMN game_type TEXT NOT NULL DEFAULT '';
ALTER TABLE active_hosts ADD COLUMN mission_name TEXT NOT NULL DEFAULT '';
ALTER TABLE active_hosts ADD COLUMN country TEXT NOT NULL DEFAULT '';
ALTER TABLE active_hosts ADD COLUMN password TEXT NOT NULL DEFAULT 'N';
ALTER TABLE active_hosts ADD COLUMN locked TEXT NOT NULL DEFAULT 'N';
ALTER TABLE active_hosts ADD COLUMN dedicated TEXT NOT NULL DEFAULT 'Y';
ALTER TABLE active_hosts ADD COLUMN stat TEXT NOT NULL DEFAULT 'N';
ALTER TABLE active_hosts ADD COLUMN exp TEXT NOT NULL DEFAULT '';
ALTER TABLE active_hosts ADD COLUMN exp_bits TEXT NOT NULL DEFAULT '';
ALTER TABLE active_hosts ADD COLUMN ver1 TEXT NOT NULL DEFAULT '';
ALTER TABLE active_hosts ADD COLUMN joicon2 TEXT NOT NULL DEFAULT '';
