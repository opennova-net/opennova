-- Active host registrations. Replaces the in-memory snapshot the standalone
-- server used to keep in nw_udp_listener::lobby_states_; persisting here
-- means /jop_2.gsb and /api/hosts can query directly via SQL, plus the
-- state survives Vue/admin requests touching the DB without locking the
-- UDP thread's std::map.
--
-- Mirrors onnet's Host model in onnw/db/models.py. Lifecycle:
--   ClientHostRequest  -> INSERT or REPLACE
--   ClientHostUpdate   -> UPDATE (refresh player_count/host_key/pcid_key/etc)
--   GOODBYE / timeout  -> DELETE
-- Also: server boot wipes the table (DELETE FROM active_hosts) so stale
-- rows from a previous run don't leak into /jop_2.gsb. UDP HELLO from the
-- previous host process won't be acked anyway, so the host shouldn't
-- reappear without re-registering.

CREATE TABLE IF NOT EXISTS active_hosts (
    rid              INTEGER PRIMARY KEY,
    gsid             TEXT NOT NULL,
    game             TEXT NOT NULL,            -- LobbyName, e.g. "jop_2_consumer"
    app_id           TEXT NOT NULL,
    server_name      TEXT NOT NULL,
    host_ip          TEXT NOT NULL,
    host_port        INTEGER NOT NULL,
    host_key         TEXT,                     -- ClientHostUpdate.HostKey
    pcid_key         TEXT,                     -- ClientHostUpdate.PCIDKey (used by /NWJoin.dll PUB encode)
    player_count     INTEGER NOT NULL DEFAULT 0,
    max_players      INTEGER NOT NULL DEFAULT 0,
    region           TEXT,
    host_user_id     INTEGER REFERENCES players(id),
    peer_ip          TEXT NOT NULL,            -- the host's UDP source IP (a.b.c.d)
    peer_port        INTEGER NOT NULL,
    created_at       TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at       TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_active_hosts_game ON active_hosts(game);
CREATE INDEX IF NOT EXISTS idx_active_hosts_peer ON active_hosts(peer_ip, peer_port);

-- Per-host player roster. ClientHostPlayerAdded inserts (correlated to a
-- recent /NWJoin.dll second call); ClientHostPlayerRemoved or the host
-- disconnect deletes. Lets /api/hosts return a "who's in this game" list
-- instead of just a player_count integer.
CREATE TABLE IF NOT EXISTS host_players (
    host_rid       INTEGER NOT NULL REFERENCES active_hosts(rid) ON DELETE CASCADE,
    user_id        INTEGER REFERENCES players(id),
    nwhandle       TEXT NOT NULL,
    peer_ip        TEXT NOT NULL,
    peer_port      INTEGER NOT NULL,
    joined_at      TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (host_rid, peer_ip, peer_port)
);

CREATE INDEX IF NOT EXISTS idx_host_players_host ON host_players(host_rid);
CREATE INDEX IF NOT EXISTS idx_host_players_peer ON host_players(peer_ip, peer_port);
