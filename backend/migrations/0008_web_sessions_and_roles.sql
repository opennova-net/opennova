-- Website login sessions and account roles: the NovaWorld site's own auth,
-- beside the shared ADMIN_API_TOKEN machine callers keep using.
--
-- web_sessions holds one row per logged-in browser. The cookie carries a
-- 32-byte random token (64 hex digits); only its SHA-256 (hex) is stored, so a
-- leaked database names no usable session. Times are UTC CURRENT_TIMESTAMP
-- text ('YYYY-MM-DD HH:MM:SS'), compared as strings against datetime('now').
-- expires_at slides forward on use (apps/novaworld_server/web_session.cpp);
-- the main tick prunes rows past it. A deleted player takes its sessions.
--
-- players.role is the site role: 'player' for every account, 'admin' for the
-- accounts the admin routes accept a session from (ONNET_BOOTSTRAP_ADMIN
-- promotes the first one at boot).
--
-- Forward-only, like 0005..0007: applies exactly once on fresh and existing DBs.

CREATE TABLE web_sessions (
    token_hash   TEXT PRIMARY KEY,
    user_id      INTEGER NOT NULL REFERENCES players(id) ON DELETE CASCADE,
    created_at   TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    expires_at   TEXT NOT NULL,
    last_seen_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    ip           TEXT NOT NULL DEFAULT '',
    user_agent   TEXT NOT NULL DEFAULT ''
);
CREATE INDEX idx_web_sessions_user_id ON web_sessions(user_id);

ALTER TABLE players ADD COLUMN role TEXT NOT NULL DEFAULT 'player'
    CHECK (role IN ('player', 'admin'));
