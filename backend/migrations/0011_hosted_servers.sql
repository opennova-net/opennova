-- Hosted game servers (ADR 0053, proposed): an approved account starts one
-- opennova-serve on a machine of its own from the site, and the service's
-- supervisor (apps/novaworld_server/hosted/) runs it until its owner stops it.
--
-- players.kind is 'player' for every account a person registers, and
-- 'service' for a hosted server's own account (srv-<id>, the hosted server's
-- id): the supervisor mints it with a random password, sets a new one at every
-- start of the server's process, and deletes it once the server has stopped or
-- failed. The server's lister logs in with it through the unchanged
-- /NWLogin.dll and /NWHost.dll, so the listing's HostKey is claimed by it
-- (host_keys); the owner-facing views and pushes treat such a listing as the
-- hosted server's owner's. The website's login refuses a service account, the
-- admin users list hides it, and no person may register a name or handle
-- starting with srv- (apps/novaworld_server/auth.h).
--
-- players.can_host is 1 once an admin approves the account for a hosted server
-- (PUT /api/admin/users/<id>/can-host).
--
-- hosted_servers holds one row per start an owner asks for, walked through
-- requested -> provisioning -> booting -> listing -> running -> stopping ->
-- stopped | failed by the supervisor (a crash restart goes back to
-- provisioning). The owner's settings are columns the backend range-checked
-- (apps/novaworld_server/hosted/hosted_settings.h); the supervisor renders the
-- server's files from them and never takes a file from the owner. An owner has
-- at most one live server (any state but stopped and failed), held by the
-- partial unique index; the global cap (ONNET_HOSTED_CAP) is counted in the
-- transaction that inserts a request. admin_user / admin_password are the
-- remote-admin login the rendered admin.cfg grants the service alone (a new
-- one at every start); they never leave the backend. machine_id is the
-- MachineProvider's handle for the machine (provider names which one).
-- owner_user_id and service_user_id are set NULL when the account goes: the
-- supervisor stops a live server whose owner is gone.
--
-- hosted_server_events keeps one row per state transition, with what caused it.
--
-- The remote-admin console's ring (hosted_server_console) comes with the admin
-- link that fills it.
--
-- Forward-only, like 0005..0010: applies exactly once on fresh and existing DBs.

ALTER TABLE players ADD COLUMN kind TEXT NOT NULL DEFAULT 'player'
    CHECK (kind IN ('player', 'service'));
ALTER TABLE players ADD COLUMN can_host INTEGER NOT NULL DEFAULT 0
    CHECK (can_host IN (0, 1));

CREATE TABLE hosted_servers (
    id               INTEGER PRIMARY KEY AUTOINCREMENT,
    owner_user_id    INTEGER REFERENCES players(id) ON DELETE SET NULL,
    service_user_id  INTEGER REFERENCES players(id) ON DELETE SET NULL,
    state            TEXT NOT NULL DEFAULT 'requested'
        CHECK (state IN ('requested', 'provisioning', 'booting', 'listing', 'running',
                         'stopping', 'stopped', 'failed')),
    -- The owner's settings, range-checked (hosted_settings.h).
    game_name        TEXT NOT NULL,
    max_players      INTEGER NOT NULL,
    game_password    TEXT NOT NULL DEFAULT '',
    time_limit       INTEGER NOT NULL,
    kill_limit       INTEGER NOT NULL,
    max_score        INTEGER NOT NULL,
    respawn          INTEGER NOT NULL,
    start_delay      INTEGER NOT NULL,
    team_ff          INTEGER NOT NULL DEFAULT 0 CHECK (team_ff IN (0, 1)),
    expansion        TEXT NOT NULL DEFAULT '',   -- '' (base Joint Operations) or 'jox01'
    missions         TEXT NOT NULL,              -- the rotation, one file name per line
    -- The machine.
    provider         TEXT NOT NULL DEFAULT '',
    machine_id       TEXT NOT NULL DEFAULT '',
    machine_address  TEXT NOT NULL DEFAULT '',
    admin_port       INTEGER NOT NULL DEFAULT 0,
    admin_user       TEXT NOT NULL DEFAULT '',
    admin_password   TEXT NOT NULL DEFAULT '',
    restarts         INTEGER NOT NULL DEFAULT 0,
    failure_reason   TEXT NOT NULL DEFAULT '',
    created_at       TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at       TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    running_at       TEXT,
    ended_at         TEXT
);
CREATE UNIQUE INDEX idx_hosted_servers_one_live ON hosted_servers(owner_user_id)
    WHERE state NOT IN ('stopped', 'failed');
CREATE INDEX idx_hosted_servers_state ON hosted_servers(state);
CREATE INDEX idx_hosted_servers_service_user ON hosted_servers(service_user_id);

CREATE TABLE hosted_server_events (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    server_id   INTEGER NOT NULL REFERENCES hosted_servers(id) ON DELETE CASCADE,
    from_state  TEXT NOT NULL,
    to_state    TEXT NOT NULL,
    detail      TEXT NOT NULL DEFAULT '',
    at          TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE INDEX idx_hosted_server_events_server ON hosted_server_events(server_id, id);
