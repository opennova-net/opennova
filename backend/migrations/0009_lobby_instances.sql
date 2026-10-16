-- Several lobby instances (apps/novaworld_server/README.md "Lobby instances";
-- engine/net/novaworld/lobby_repository.h): each edge process runs one lobby (a
-- NovaWorld UDP listener with its own socket and its sessions in its memory
-- alone), and the gate hands each client one of the instances that take new
-- clients. Service policy, not a port.
--
-- An instance is its advertised endpoint (ONNET_LOBBY_ENDPOINT, the gate's
-- udpnovaworld for the clients it hands there): the edge's lobby registers it
-- at its start, under the row its endpoint already has (a restart of the same
-- edge keeps its id, and its start clears that id's rows: the previous run's
-- sessions, commands and listings), moves last_seen_at every few seconds while
-- it runs, and deletes it at a clean stop. An instance whose last_seen_at is
-- five minutes old (well past a database failover) ran on a process that is
-- gone: any edge's start, and the hosts.prune job, delete it, and its rows go
-- with it (ON DELETE CASCADE); a lobby that finds its own row so reaped
-- registers again and writes its rows anew. state 'draining'
-- (an admin's, or the edge's own as it stops) takes no new client from any gate;
-- the sessions it holds stay until they leave.
CREATE TABLE lobby_instances (
    id           bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    endpoint     text NOT NULL UNIQUE CHECK (endpoint <> ''),
    state        text NOT NULL DEFAULT 'active' CHECK (state IN ('active', 'draining')),
    started_at   timestamptz NOT NULL DEFAULT now(),
    last_seen_at timestamptz NOT NULL DEFAULT now()
);

-- Each row the lobby writes names its instance: its sessions, the commands
-- addressed to them (copied from the session row as the command is inserted;
-- forget_owner, which names no session, one row per instance), and its
-- listings. A lobby that ran before instances wrote NULL; an edge's start
-- clears those too, since no running lobby holds them.
ALTER TABLE lobby_sessions
    ADD CONSTRAINT lobby_sessions_lobby_id_fkey
        FOREIGN KEY (lobby_id) REFERENCES lobby_instances(id) ON DELETE CASCADE;
ALTER TABLE lobby_commands
    ADD CONSTRAINT lobby_commands_lobby_id_fkey
        FOREIGN KEY (lobby_id) REFERENCES lobby_instances(id) ON DELETE CASCADE;
ALTER TABLE active_hosts
    ADD COLUMN lobby_id bigint REFERENCES lobby_instances(id) ON DELETE CASCADE;
CREATE INDEX idx_lobby_sessions_lobby ON lobby_sessions(lobby_id);
CREATE INDEX idx_lobby_commands_lobby ON lobby_commands(lobby_id, id);
CREATE INDEX idx_active_hosts_lobby ON active_hosts(lobby_id);

-- The host RIDs, one sequence for every instance, so two never mint the same
-- (lobby_session.cpp mint_rid: 0x0A000000 | the sequence's next value, 1 to
-- 0xFFFFFF, cycling, a value a listing or a session row still holds passed
-- over: lobbydb::next_rid). A RID is never reused short of the cycle, so a claim on
-- a RID that no listing holds is free (hostdb::claim_host_key) and no boot
-- resets the claims. It starts past every RID a lobby before it minted.
CREATE SEQUENCE lobby_rid_seq AS bigint MINVALUE 1 MAXVALUE 16777215 CYCLE;
SELECT setval('lobby_rid_seq', GREATEST(
    (SELECT COALESCE(MAX(rid & 16777215), 0) FROM active_hosts),
    (SELECT COALESCE(MAX(rid & 16777215), 0) FROM lobby_sessions)) % 16777215 + 1, false);
