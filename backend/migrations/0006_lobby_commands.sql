-- The lobby's state, for whatever is not the lobby (apps/novaworld_server/README.md
-- "Connection lifecycle", "Pushing statements to a listed server";
-- engine/net/novaworld/lobby_repository.h): the NovaWorld sessions the lobby
-- holds, which the lobby alone writes and the website's routes read, and the
-- commands the routes and the hosted servers' supervisor address to the lobby
-- that holds a session, which that lobby alone takes and runs. The NovaWorld
-- UDP listener owns its sessions and its socket; nothing else reaches its
-- memory, so these two tables are the only way in and out. Service policy, not
-- a port.
--
-- One lobby runs today and writes NULL in both tables' lobby_id. Once several
-- lobby instances run, each its own process with its own socket, a table of
-- them (lobby_instances) names each one and lobby_id becomes a reference to it
-- (ON DELETE CASCADE): an instance writes its own id on its session rows, a
-- command takes the lobby_id of the session it names as it is inserted (the
-- insert copies it from the session row, so the routes never pick an instance),
-- an instance takes only the commands bearing its id, and an instance's boot
-- clears only its own rows. A command that names no session (forget_owner)
-- goes to every instance: one row per live instance, each bearing that
-- instance's id.

-- A NovaWorld UDP session the lobby holds: one row per logical connection,
-- from the ClientHello that admits it (state 'handshaking') to its drop (a
-- goodbye, the 240 s receive silence, a Hello that replaces it, an admin's end,
-- the shutdown), when the lobby deletes it. A ClientAuth that is not the
-- retransmit of the accepted one starts a new logical connection on the same
-- address, so its row replaces the Hello's (or the previous one's) under a new
-- id; a command addressed to the old row goes with it.
--
-- The connection's registry half: connection_id is the registry's id for it
-- (the ClientHello's CI, or one the registry synthesized when another address
-- holds that CI; unique among one lobby's sessions), pn the latest Hello's
-- protocol name, identity the accepted ClientAuth's NA (the game tag,
-- 'jop:cus2'), connected_at the Hello that admitted the address. last_seen_at
-- is the latest packet heard from it, written every ten seconds at most, so it
-- lags by as much. The lobby half: login_session_tag the LOGINSESSIONTAG its
-- latest Cookie list carried (the retail login's, active_user_sessions; a
-- secret, no route sends it), rid the RID it holds (minted by its first
-- ClientHostRequest, kept after its hosting ends and across its reconnect),
-- hosting whether it hosts now, listed whether it is in the browser (hosting
-- and not held out for a HostKey naming an account), host_user_id the account
-- its HostKey named (the lobby's own view of the listing's owner; no foreign
-- key, so an account's delete leaves it for the forget_owner command to clear),
-- and the listing's name, map and players as the host last reported them.
CREATE TABLE lobby_sessions (
    id                bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    lobby_id          bigint,
    connection_id     bigint NOT NULL CHECK (connection_id BETWEEN 0 AND 4294967295),
    peer_ip           text NOT NULL,
    peer_port         bigint NOT NULL CHECK (peer_port BETWEEN 0 AND 65535),
    state             text NOT NULL CHECK (state IN ('handshaking', 'active', 'closing')),
    pn                text NOT NULL DEFAULT '',
    identity          text NOT NULL DEFAULT '',
    login_session_tag text NOT NULL DEFAULT '',
    rid               bigint CHECK (rid BETWEEN 1 AND 4294967295),
    hosting           boolean NOT NULL DEFAULT false,
    listed            boolean NOT NULL DEFAULT false,
    host_user_id      bigint,
    server_name       text NOT NULL DEFAULT '',
    mission_name      text NOT NULL DEFAULT '',
    players           bigint NOT NULL DEFAULT 0,
    max_players       bigint NOT NULL DEFAULT 0,
    connected_at      timestamptz NOT NULL,
    last_seen_at      timestamptz NOT NULL,
    CHECK (listed <= hosting)
);
CREATE INDEX idx_lobby_sessions_connection ON lobby_sessions(connection_id);
CREATE INDEX idx_lobby_sessions_rid ON lobby_sessions(rid) WHERE rid IS NOT NULL;

-- A command addressed to the lobby that holds a session, inserted by an HTTP
-- route or the hosted servers' supervisor once its own checks pass, and taken
-- (deleted as it is read) and run by that lobby's receive thread, which polls
-- for them every 250 ms. A command is best effort, as the lobby's own pushes
-- are: one whose session is gone by then, or no longer what the command was
-- checked against, is dropped, and so is one past its expires_at (the lobby
-- was not taking commands). The kinds:
--
--   server_command  a ServerCommand statement (cmd: the Cmd line, in the
--                   host's cp1252 bytes) to the hosting session session_id;
--   stop_hosting    a ServerStopHosting statement (msg_code, msg_param1,
--                   msg_param2) to the hosting session session_id, after which
--                   the listing leaves the browser;
--   end_session     the admin's end of the session session_id: its
--                   ServerGoodBye burst, then its drop;
--   forget_owner    account_id loses its keys (a password reset, a status
--                   other than 'active', a delete): the lobby takes it off
--                   every session and parked reconnect that names it as its
--                   listing's owner. It names no session.
--
-- rid is the RID a push resolved to, and account_id on a push scopes it to the
-- listing's owner (the owner's routes, the supervisor's stop of a hosted
-- server, held by its service account; NULL for an admin's push): the lobby
-- sends it only while that account still owns the listing, in its memory and
-- by the live claim (host_keys).
CREATE TABLE lobby_commands (
    id         bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    lobby_id   bigint,
    kind       text NOT NULL
        CHECK (kind IN ('server_command', 'stop_hosting', 'end_session', 'forget_owner')),
    session_id bigint REFERENCES lobby_sessions(id) ON DELETE CASCADE,
    rid        bigint CHECK (rid BETWEEN 1 AND 4294967295),
    account_id bigint,
    cmd        text,
    msg_code   bigint,
    msg_param1 bigint,
    msg_param2 bigint,
    created_at timestamptz NOT NULL DEFAULT now(),
    expires_at timestamptz NOT NULL,
    CHECK ((session_id IS NULL) = (kind = 'forget_owner')),
    CHECK (kind <> 'server_command' OR (cmd IS NOT NULL AND rid IS NOT NULL)),
    CHECK (kind <> 'stop_hosting' OR (msg_code IS NOT NULL AND msg_param1 IS NOT NULL
                                      AND msg_param2 IS NOT NULL AND rid IS NOT NULL)),
    CHECK (kind <> 'forget_owner' OR account_id IS NOT NULL)
);
CREATE INDEX idx_lobby_commands_session ON lobby_commands(session_id);
