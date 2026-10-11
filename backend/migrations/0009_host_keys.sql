-- The hosting keys /NWHost.dll mints, each with the account it was minted for:
-- the link from a listed server (active_hosts.host_user_id) to the account that
-- hosted it.
--
-- /NWHost.dll mints a 48-letter A..P HOSTKEY (24 random bytes, a nibble each)
-- into the page a stock client reads back, and the client copies it into its
-- Host var list (the first ClientHostUpdate carries it; a re-host's
-- ClientHostRequest too). When the /NWHost.dll request's own retail login names
-- an account (apps/novaworld_server/http_listener.cpp), the key is recorded
-- here; only its SHA-256 (hex) is stored, the way web_sessions keeps its
-- tokens, so this table names no usable key. The lobby looks a presented key up
-- by that hash and claims it for the connection's RID (claimed_rid): a key
-- backs one listing at a time, so another connection presenting it while that
-- listing lives links to nothing
-- (engine/net/novaworld/service/host_repository.cpp, claim_host_key).
--
-- A key lasts 24 h from its last use: its claim, or the main tick's sweep
-- while a listed server holds it. The sweep deletes the rest. A deleted player
-- takes its keys. Times are UTC CURRENT_TIMESTAMP text
-- ('YYYY-MM-DD HH:MM:SS'), compared as strings against datetime('now').
--
-- Landed with 0001 (players.id AUTOINCREMENT) and 0002 (ON DELETE SET NULL on
-- active_hosts.host_user_id and host_players.user_id) edited in place, which a
-- database already past them never re-runs (migrations are tracked by file
-- name): the service database is reset on that deploy.

CREATE TABLE host_keys (
    key_hash     TEXT PRIMARY KEY,
    user_id      INTEGER NOT NULL REFERENCES players(id) ON DELETE CASCADE,
    claimed_rid  INTEGER,
    created_at   TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_used_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE INDEX idx_host_keys_user_id ON host_keys(user_id);
CREATE INDEX idx_host_keys_last_used_at ON host_keys(last_used_at);
