-- The NovaWorld service's schema (PostgreSQL 17): every table the service
-- programs read and write, and the rows they start from. Applied once per
-- database by opennova::db::run_migrations (database.h), which records this
-- file's SHA-256. Until 1.0 the database is disposable and this one file is the
-- schema: a change edits it in place, and a database that applied the old file
-- is reset when it takes the change (backend/README.md "Migrations until 1.0").
-- Service policy, not a port.
--
-- Dialect rules:
--   - integers are bigint (an id, a count, a port, a RID: a u32 RID does not
--     fit an integer); a flag is boolean
--   - an id is bigint GENERATED ALWAYS AS IDENTITY: a deleted row's id is never
--     handed out again, so nothing that kept one (a listing's host_user_id in a
--     lobby's memory, a log line) ever names a newcomer
--   - a time is timestamptz, written now() (the transaction's start); the
--     service reads it as "YYYY-MM-DD HH:MM:SS" UTC text
--   - the database is SQL_ASCII with C collation and ctype (run_migrations
--     checks): text is the bytes a client sent (a stock client's cp1252
--     included), compared and ordered byte by byte, lower() ASCII only

-- ===========================================================================
-- Accounts, the catalog and the listings
-- ===========================================================================

-- Players (accounts). kind 'service' is a hosted server's own account
-- (srv-<id>), minted by the supervisor with a random password, set again at
-- every start of the server's process and deleted once the server has stopped
-- or failed; the website's login refuses one, the admin users list hides it,
-- and no person may register a name or handle starting with srv-
-- (apps/novaworld_common/accounts/auth.h). account_status is 'active' or the
-- name of whatever else the account is ('banned', ...), never empty. role is
-- the website role. credentials_gen moves (+1) with every admin update that
-- sets a password or a status other than 'active' (auth.cpp update_user), in
-- the statement that makes the change: every login's grants (a website session,
-- the retail login's active_user_sessions row, a known login address) are
-- written only while the account still has the generation its password check
-- read. An account's hosting entitlement comes from its support tier and its
-- dedicated machines (billing_subscriptions) and an admin's grants (comp_grants;
-- apps/novaworld_common/billing/entitlement.h).
--
-- email is the address a player registers with, which the welcome mail and the
-- password reset links go to (apps/novaworld_common/README.md "Account email and
-- password resets"), as normalize_email (auth.h) stores it: its ASCII letters
-- lowercased (lower() folds ASCII alone under the C ctype, the same fold), so
-- one address names one account however it was typed. NULL for an account with
-- none: one an admin made without one, a hosted server's service account.
CREATE TABLE players (
    id              bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    username        text NOT NULL UNIQUE,
    password_hash   text NOT NULL,
    pcid            text NOT NULL UNIQUE,
    nwh             text NOT NULL,
    nwhandle        text NOT NULL,
    created_at      timestamptz NOT NULL DEFAULT now(),
    last_login      timestamptz,
    account_status  text NOT NULL DEFAULT 'active' CHECK (account_status <> ''),
    role            text NOT NULL DEFAULT 'player' CHECK (role IN ('player', 'admin')),
    credentials_gen bigint NOT NULL DEFAULT 0,
    kind            text NOT NULL DEFAULT 'player' CHECK (kind IN ('player', 'service')),
    email           text CHECK (email IS NULL OR (email <> '' AND email = lower(email)))
);
CREATE INDEX idx_players_nwhandle ON players(nwhandle);
CREATE UNIQUE INDEX idx_players_email ON players(email);

-- Games: one row per lobby (apps/novaworld_common/program/catalog_repository.h).
-- A client's gate tag selects its row, which decides the gate's answer (the
-- LobbyName, the startupurl, the GLSVSS leg), the .gsb list its browser
-- fetches and the game its listings land under. The rows are the seed's
-- (backend/seed/0001_games.sql). title is the engine's game profile code the
-- row's clients speak (engine/base/gameprofile: 'jo', 'dfx2');
-- startup_query the gate's startupurl after "nwprepare.dll?"; glsvss_* the
-- gate's GLSVSSREQUEST / GLSVSSRIMS / GLSVSSAGRMS (an empty request sends
-- none: the service's configured ONNET_GLSVSS_* stand in).
CREATE TABLE games (
    id             bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    slug           text NOT NULL UNIQUE,
    display_name   text NOT NULL,
    lobby_name     text NOT NULL,
    gate_tag       text NOT NULL UNIQUE,
    start_page     text NOT NULL,
    host_page      text NOT NULL,
    gsb_path       text NOT NULL,
    join_lan_url   text,
    ver1           text NOT NULL,
    ver2           text NOT NULL,
    exp_bits       text NOT NULL,
    pfid           text,
    created_at     timestamptz NOT NULL DEFAULT now(),
    updated_at     timestamptz NOT NULL DEFAULT now(),
    title          text NOT NULL DEFAULT '',
    startup_query  text NOT NULL DEFAULT '',
    glsvss_request text NOT NULL DEFAULT '',
    glsvss_rims    bigint NOT NULL DEFAULT 0,
    glsvss_agrms   bigint NOT NULL DEFAULT 0
);

-- The lobby instances (apps/novaworld_common/README.md "Lobby instances";
-- engine/net/novaworld/lobby_repository.h): each edge process runs one lobby (a
-- NovaWorld UDP listener with its own socket and its sessions in its memory
-- alone), and the gate hands each client one of the instances that take new
-- clients. Every lobby announces the deployment's one lobby address, the load
-- balancer's (on ECS: the gate's Elastic IP and UDP 64206), which spreads the
-- clients across the lobby tasks, so a stock host whose lobby went away
-- reconnects to that same address and lands on a live one. An instance is its
-- id alone, and its endpoint (ONNET_LOBBY_ENDPOINT) is data: every start of a
-- lobby registers a row of its own, moves last_seen_at every few seconds while
-- it runs, and deletes it at a clean stop. An instance whose last_seen_at is
-- five minutes old (well past a database failover) ran on a process that is
-- gone: any edge's start, and the hosts.prune job, delete it, and its rows go
-- with it (ON DELETE CASCADE); a lobby that finds its own row so reaped
-- registers again and writes its rows anew. state 'draining' (an admin's, or
-- the edge's own as it stops) takes no new client from any gate; the sessions
-- it holds stay until they leave. task_id is the task the instance's lobby runs
-- as (program_usage.h find_usage_task: its ECS task id, else its hostname), the
-- key its program_samples rows carry, so the admin's view links an instance to
-- its usage; '' for none.
CREATE TABLE lobby_instances (
    id           bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    endpoint     text NOT NULL CHECK (endpoint <> ''),
    state        text NOT NULL DEFAULT 'active' CHECK (state IN ('active', 'draining')),
    started_at   timestamptz NOT NULL DEFAULT now(),
    last_seen_at timestamptz NOT NULL DEFAULT now(),
    task_id      text NOT NULL DEFAULT ''
);

-- The listed game servers (the lobby browser, /jop_2.gsb, /api/hosts). A
-- ClientHostRequest upserts its row, a ClientHostUpdate refreshes it, GOODBYE,
-- a stop or the heartbeat timeout deletes it, and its lobby instance's start
-- or reap takes it (lobby_id). The text columns are the host's Host var list
-- as it sent them (a stock host's cp1252 bytes); host_key / pcid_key are its
-- HostKey and PCIDKey. host_user_id is the owner: the account while one of its
-- hosting keys stands claimed for this RID (host_keys), else NULL.
CREATE TABLE active_hosts (
    rid          bigint PRIMARY KEY,
    gsid         text NOT NULL,
    game         text NOT NULL,            -- LobbyName, e.g. "jop_2_consumer"
    app_id       text NOT NULL,
    server_name  text NOT NULL,
    host_ip      text NOT NULL,
    host_port    bigint NOT NULL,
    host_key     text,
    pcid_key     text,
    player_count bigint NOT NULL DEFAULT 0,
    max_players  bigint NOT NULL DEFAULT 0,
    region       text,
    host_user_id bigint REFERENCES players(id) ON DELETE SET NULL,
    peer_ip      text NOT NULL,            -- the host's UDP source IP (a.b.c.d)
    peer_port    bigint NOT NULL,
    created_at   timestamptz NOT NULL DEFAULT now(),
    updated_at   timestamptz NOT NULL DEFAULT now(),
    game_type    text NOT NULL DEFAULT '',
    mission_name text NOT NULL DEFAULT '',
    country      text NOT NULL DEFAULT '',
    password     text NOT NULL DEFAULT 'N',
    locked       text NOT NULL DEFAULT 'N',
    dedicated    text NOT NULL DEFAULT 'Y',
    stat         text NOT NULL DEFAULT 'N',
    exp          text NOT NULL DEFAULT '',
    exp_bits     text NOT NULL DEFAULT '',
    ver1         text NOT NULL DEFAULT '',
    joicon2      text NOT NULL DEFAULT '',
    time_left    text NOT NULL DEFAULT '',
    time_of_day  text NOT NULL DEFAULT '',
    msg          text NOT NULL DEFAULT '',
    mod          text NOT NULL DEFAULT '',
    age          text NOT NULL DEFAULT '',
    pb_server    text NOT NULL DEFAULT '',
    level_range  text NOT NULL DEFAULT '',
    bb_mode      text NOT NULL DEFAULT '',
    skins        text NOT NULL DEFAULT '',
    tracers      text NOT NULL DEFAULT '',
    pix          text NOT NULL DEFAULT '',
    lobby_id     bigint REFERENCES lobby_instances(id) ON DELETE CASCADE
);
CREATE INDEX idx_active_hosts_game ON active_hosts(game);
CREATE INDEX idx_active_hosts_peer ON active_hosts(peer_ip, peer_port);
CREATE INDEX idx_active_hosts_host_key ON active_hosts(host_key);
CREATE INDEX idx_active_hosts_host_user ON active_hosts(host_user_id);
CREATE INDEX idx_active_hosts_lobby ON active_hosts(lobby_id);

-- A listing's joiners (/NWJoin.dll's rows, which GET /api/lobbies/<rid> lists
-- as its roster), keyed by the joiner's account: the one its retail login
-- names and its lobby session signed in as, one row per account and listing
-- (its latest join: its handle and the HTTP address it came from). The lobby
-- takes the row off when that session stops playing there, plays elsewhere or
-- ends (hostdb::remove_player); it goes with its listing and with its account.
CREATE TABLE host_players (
    host_rid  bigint NOT NULL REFERENCES active_hosts(rid) ON DELETE CASCADE,
    user_id   bigint NOT NULL REFERENCES players(id) ON DELETE CASCADE,
    nwhandle  text NOT NULL,
    peer_ip   text NOT NULL,
    joined_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (host_rid, user_id)
);
CREATE INDEX idx_host_players_user ON host_players(user_id);

-- The host-reported roster: the PlayerList's VarFNum-indexed entries, one
-- slot per player (Server_PlayerAdd @0x51d441..0x51d4aa: PlayerName,
-- PlayerIpAndPort, PlayerPCID, PlayerTeam, PlayerType), so the GSB row tail
-- carries the names the retail browser counts
-- (NapiGameList_ProcessEncryptedResponse @0x63dafc).
CREATE TABLE host_roster (
    host_rid    bigint NOT NULL REFERENCES active_hosts(rid) ON DELETE CASCADE,
    slot        bigint NOT NULL,           -- the ClientVar VarFNum
    player_name text NOT NULL DEFAULT '',
    ip_and_port text NOT NULL DEFAULT '',
    pcid        text NOT NULL DEFAULT '',
    team        text NOT NULL DEFAULT '',
    type        text NOT NULL DEFAULT '',
    PRIMARY KEY (host_rid, slot)
);

-- An account's access to a game and the expansion bits it owns there; an
-- account with no row owns the game's own exp_bits.
CREATE TABLE player_game_access (
    user_id    bigint NOT NULL REFERENCES players(id) ON DELETE CASCADE,
    game_slug  text NOT NULL REFERENCES games(slug) ON DELETE CASCADE,
    status     text NOT NULL DEFAULT 'active',
    exp_bits   text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, game_slug)
);
CREATE INDEX idx_player_game_access_game ON player_game_access(game_slug);

-- The service's maintenance switch and the message a client gets while it is
-- on: one row.
CREATE TABLE server_status (
    id                  bigint PRIMARY KEY CHECK (id = 1),
    maintenance_enabled boolean NOT NULL DEFAULT false,
    message             text NOT NULL DEFAULT 'NovaWorld is temporarily unavailable.',
    updated_at          timestamptz NOT NULL DEFAULT now()
);
INSERT INTO server_status (id, maintenance_enabled, message)
VALUES (1, false, 'NovaWorld is temporarily unavailable.');

-- The retail login's session: its LOGINSESSIONTAG names the account to
-- /NWHost.dll, /NWJoin.dll and /NWAccount.dll only while credentials_gen (the
-- generation the login was made under) is still the account's. The boot
-- wipes the table.
CREATE TABLE active_user_sessions (
    user_id         bigint PRIMARY KEY REFERENCES players(id) ON DELETE CASCADE,
    username        text NOT NULL,
    session_tag     text NOT NULL,
    persistent_id   text,
    remote_ip       text,
    user_agent      text,
    created_at      timestamptz NOT NULL DEFAULT now(),
    last_seen_at    timestamptz NOT NULL DEFAULT now(),
    credentials_gen bigint NOT NULL DEFAULT 0
);
CREATE INDEX idx_active_user_sessions_tag ON active_user_sessions(session_tag);
CREATE INDEX idx_active_user_sessions_persistent ON active_user_sessions(persistent_id);

-- Every inbound signal the service has no handler for (an unhandled NW-UDP
-- opcode, a non-NOVAWORLDUDP PN, an unknown container/protocol message or
-- gate tag, an HTTP 404), deduped by (channel, signature) by the in-memory
-- UnknownTracker and flushed on the main tick: the count, first/last seen
-- (ms) and the first sighting's sample, capped at 512 bytes.
CREATE TABLE unknown_messages (
    id            bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    channel       text NOT NULL,            -- gate | nwu | container | ptype | pn | http
    signature     text NOT NULL,            -- e.g. "0x47", "ClientFooRequest", "GET /foo"
    count         bigint NOT NULL DEFAULT 0,
    first_seen_ms bigint NOT NULL,
    last_seen_ms  bigint NOT NULL,
    sample        bytea,
    sample_meta   text,                     -- e.g. peer "1.2.3.4:64206"
    UNIQUE (channel, signature)
);

-- The website's login sessions: one row per logged-in browser. The cookie
-- carries a 32-byte random token; only its SHA-256 (hex) is stored, so a
-- copy of the database names no usable session. expires_at slides forward on
-- use (apps/novaworld_common/accounts/web_session.h); the main tick prunes rows
-- past it. id orders sessions minted at one instant.
CREATE TABLE web_sessions (
    token_hash   text PRIMARY KEY,
    id           bigint GENERATED ALWAYS AS IDENTITY UNIQUE,
    user_id      bigint NOT NULL REFERENCES players(id) ON DELETE CASCADE,
    created_at   timestamptz NOT NULL DEFAULT now(),
    expires_at   timestamptz NOT NULL,
    last_seen_at timestamptz NOT NULL DEFAULT now(),
    ip           text NOT NULL DEFAULT '',
    user_agent   text NOT NULL DEFAULT ''
);
CREATE INDEX idx_web_sessions_user_id ON web_sessions(user_id);
CREATE INDEX idx_web_sessions_expires_at ON web_sessions(expires_at);

-- Per account, the client addresses (an IPv6 address grouped to its /64)
-- that logged in with the right password, on the site or the retail login,
-- for 30 days: the login brake's per-username cap spares them, so a
-- stranger's failed logins cannot keep the owner out.
CREATE TABLE login_addresses (
    user_id         bigint NOT NULL REFERENCES players(id) ON DELETE CASCADE,
    address         text NOT NULL,
    id              bigint GENERATED ALWAYS AS IDENTITY UNIQUE,
    last_success_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, address)
);
CREATE INDEX idx_login_addresses_last_success_at ON login_addresses(last_success_at);

-- The hosting keys /NWHost.dll mints (a 48-letter A..P HOSTKEY a stock client
-- copies into its Host var list), each with the account it was minted for:
-- the link from a listed server to its owner. Only the key's SHA-256 (hex) is
-- stored. The lobby claims a presented key for the connection's RID
-- (claimed_rid): a key backs one listing at a time. A key lasts 24 h from its
-- last use (its claim, or the main tick's sweep while a listed server holds
-- it); the sweep deletes the rest
-- (engine/net/novaworld/service/host_repository.cpp).
CREATE TABLE host_keys (
    key_hash     text PRIMARY KEY,
    id           bigint GENERATED ALWAYS AS IDENTITY UNIQUE,
    user_id      bigint NOT NULL REFERENCES players(id) ON DELETE CASCADE,
    claimed_rid  bigint,
    created_at   timestamptz NOT NULL DEFAULT now(),
    last_used_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX idx_host_keys_user_id ON host_keys(user_id);
CREATE INDEX idx_host_keys_last_used_at ON host_keys(last_used_at);
CREATE INDEX idx_host_keys_claimed_rid ON host_keys(claimed_rid);

-- The join tickets /NWJoin.dll mints (a joiner's PUBJOINTICKET, "JT:" and 48
-- hex digits), each bound to the joiner's account, the listing's RID and the
-- address the request came from, live until expires_at (5 minutes). A host the
-- service asked for tickets (HostRequiresJoinTicket 1) forwards the joiner's
-- in its ClientPlayerEnterRequest, and the lobby redeems it once: 6001 for one
-- unknown, expired or another listing's, 6002 for one minted at another
-- address, a joiner behind the host's own NAT excepted
-- (engine/net/novaworld/host_repository.h "The join tickets"). Only the
-- ticket's SHA-256 (hex) is stored. Each mint deletes the expired rows.
CREATE TABLE join_tickets (
    ticket_hash text PRIMARY KEY,
    host_rid    bigint NOT NULL,
    user_id     bigint NOT NULL REFERENCES players(id) ON DELETE CASCADE,
    peer_ip     text NOT NULL,
    created_at  timestamptz NOT NULL DEFAULT now(),
    expires_at  timestamptz NOT NULL
);
CREATE INDEX idx_join_tickets_expires ON join_tickets(expires_at);
CREATE INDEX idx_join_tickets_user ON join_tickets(user_id);

-- A password reset an account asked for (POST /api/auth/forgot): the SHA-256 of
-- its link's token (32 random bytes, as 64 hex digits; the link alone carries the
-- token), live until expires_at (an hour). One per account: a newer request
-- replaces it, and POST /api/auth/reset deletes it as it sets the password, so a
-- link works once. Pruned by the main tick's sweep once expired.
CREATE TABLE password_resets (
    user_id    bigint PRIMARY KEY REFERENCES players(id) ON DELETE CASCADE,
    token_hash text NOT NULL UNIQUE,
    created_at timestamptz NOT NULL DEFAULT now(),
    expires_at timestamptz NOT NULL
);
CREATE INDEX idx_password_resets_expires ON password_resets(expires_at);

-- ===========================================================================
-- The lobby's state (apps/novaworld_common/README.md "Connection lifecycle",
-- "Pushing statements to a listed server"; engine/net/novaworld/lobby_repository.h)
-- ===========================================================================
--
-- The NovaWorld sessions each lobby instance holds, which that lobby alone
-- writes and the website's routes read, and the commands the routes and the
-- hosted servers' supervisor address to the lobby that holds a session, which
-- that lobby alone takes and runs. The NovaWorld UDP listener owns its sessions
-- and its socket; nothing else reaches its memory, so these tables are the only
-- way in and out. Each row names its instance (lobby_id): an instance writes its
-- own id on its session rows, a command takes the lobby_id of the session it
-- names as it is inserted (the insert copies it from the session row, so the
-- routes never pick an instance), an instance takes only the commands bearing
-- its id, and an instance's start clears only its own rows. A command that
-- names no session (forget_owner) goes to every instance: one row per live
-- instance, each bearing that instance's id.

-- A NovaWorld UDP session a lobby holds: one row per logical connection, from
-- the ClientHello that admits it (state 'handshaking') to its drop (a goodbye,
-- the 240 s receive silence, a Hello that replaces it, an admin's end, the
-- shutdown), when the lobby deletes it. A ClientAuth that is not the
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
-- lags by as much. The lobby half: rid the RID it holds (minted by its first
-- ClientHostRequest, kept after its hosting ends and across its reconnect),
-- hosting whether it hosts now, listed whether it is in the browser (hosting
-- and not held out for a HostKey naming an account), host_user_id the account
-- its HostKey named (the lobby's own view of the listing's owner; no foreign
-- key, so an account's delete leaves it for the forget_owner command to clear),
-- and the listing's name, map and players as the host last reported them.
--
-- user_id is the account the session signed in as: the one whose live retail
-- login its Cookie list's LOGINSESSIONTAG named when the lobby first resolved
-- it (active_user_sessions), kept for the session's life however that login
-- ends, and moved only by a later tag that names another account. No foreign
-- key, as host_user_id has none: the lobby writes it from its memory, and an
-- account's delete leaves an id no player has, which the view shows as no one.
-- The tag itself, a secret, stays in the lobby's memory.
--
-- The play columns are its latest ClientPlayRequest's PlaySetup, the join
-- address the .joi handed the client: play_addr its IpAddress:PortNumber,
-- playing_rid the listing at that address when the request came (active_hosts
-- host_ip and host_port; NULL for none: a LAN server, or one no lobby lists),
-- and playing_since when it began playing there. All three clear on its
-- ClientStopPlaying, and go with the session row at its end. The admin's live
-- view (apps/novaworld_web/connection_routes.h) reads them.
CREATE TABLE lobby_sessions (
    id                bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    lobby_id          bigint REFERENCES lobby_instances(id) ON DELETE CASCADE,
    connection_id     bigint NOT NULL CHECK (connection_id BETWEEN 0 AND 4294967295),
    peer_ip           text NOT NULL,
    peer_port         bigint NOT NULL CHECK (peer_port BETWEEN 0 AND 65535),
    state             text NOT NULL CHECK (state IN ('handshaking', 'active', 'closing')),
    pn                text NOT NULL DEFAULT '',
    identity          text NOT NULL DEFAULT '',
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
    user_id           bigint,
    playing_rid       bigint CHECK (playing_rid BETWEEN 1 AND 4294967295),
    play_addr         text NOT NULL DEFAULT '',
    playing_since     timestamptz,
    CHECK (listed <= hosting)
);
CREATE INDEX idx_lobby_sessions_connection ON lobby_sessions(connection_id);
CREATE INDEX idx_lobby_sessions_rid ON lobby_sessions(rid) WHERE rid IS NOT NULL;
CREATE INDEX idx_lobby_sessions_lobby ON lobby_sessions(lobby_id);

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
    lobby_id   bigint REFERENCES lobby_instances(id) ON DELETE CASCADE,
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
CREATE INDEX idx_lobby_commands_lobby ON lobby_commands(lobby_id, id);

-- The host RIDs, one sequence for every instance, so two never mint the same
-- (lobby_session.cpp mint_rid: 0x0A000000 | the sequence's next value, 1 to
-- 0xFFFFFF, cycling, a value a listing or a session row still holds passed
-- over: lobbydb::next_rid). A RID is never reused short of the cycle, so a
-- claim on a RID that no listing holds is free (hostdb::claim_host_key) and no
-- boot resets the claims.
CREATE SEQUENCE lobby_rid_seq AS bigint MINVALUE 1 MAXVALUE 16777215 CYCLE;

-- The goodbyes a lobby instance's clean stop leaves for the sessions it held
-- ("Lobby instances"). A NAPI server that stops sends each of its connections
-- the ServerGoodBye burst, and a stock client that gets one reconnects at once.
-- On ECS the load balancer has ended a lobby task's flows by the time ECS
-- signals its stop, so a burst from the stopping task no longer reaches its
-- sessions through the lobby address: each session's next packet reaches
-- another lobby instance, which holds no session for it, and a stock client
-- left unanswered drops its session only after 240 s of receive silence. So
-- the stopping lobby leaves a row per session it held: its address (peer_ip,
-- peer_port), the key its packets address (server_sk, the SK the lobby minted,
-- every SESSION packet's session id) and the key a goodbye to it bears
-- (client_ck, the client's CK). The lobby that a packet from that address
-- carrying that SK reaches sends the goodbye in the stopped one's place. A row
-- is good for that 240 s (past it the client has reconnected by itself); each
-- stop's write deletes the older ones, and a later stop's row at the same
-- address replaces the earlier.
CREATE TABLE lobby_goodbyes (
    peer_ip   text NOT NULL,
    peer_port bigint NOT NULL CHECK (peer_port BETWEEN 0 AND 65535),
    server_sk bigint NOT NULL CHECK (server_sk BETWEEN 1 AND 4294967295),
    client_ck bigint NOT NULL CHECK (client_ck BETWEEN 0 AND 4294967295),
    left_at   timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (peer_ip, peer_port)
);

-- ===========================================================================
-- CD keys
-- ===========================================================================

-- The keys NovaWorld issues for a title whose client activates against the
-- service, and the machines each one activated. Only Delta Force Xtreme 2
-- does: its boot and menus gate on a registry CDKIID that NovaLogic's
-- NovaWorld handed out as the NWACT browser variable, bound to the machine's
-- NWPSSK fingerprint, and its LAN host checks the wire CDKIID that value
-- decodes to (docs/titles/dfx2-vs-jo-re.md section 9, DFX2-13). A key carries
-- its title (games.title), its serial (the plaintext, "DFX2-XXXX-XXXX-XXXX-
-- XXXX") and its wire key (the A-P CDKIID the serial encodes to); the account
-- it is on (NULL until assigned), who made it ('admin <name>', 'token', or
-- 'auto' for a key minted at an account's first activation), when it was
-- assigned and revoked (a revoked key activates no machine). An account holds
-- at most one live key per title. An activation is one machine a key
-- activated: the NWPSSK its client posts and the NWACT the service handed it,
-- the same every time that machine asks again (apps/novaworld_web/cd_keys.h).
CREATE TABLE cd_keys (
    id          bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    title       text NOT NULL,
    serial      text NOT NULL UNIQUE,
    wire_key    text NOT NULL UNIQUE,
    user_id     bigint REFERENCES players(id) ON DELETE CASCADE,
    created_by  text NOT NULL,
    created_at  timestamptz NOT NULL DEFAULT now(),
    assigned_at timestamptz,
    revoked_at  timestamptz
);
CREATE INDEX idx_cd_keys_title ON cd_keys(title);
CREATE INDEX idx_cd_keys_user ON cd_keys(user_id);
CREATE UNIQUE INDEX idx_cd_keys_live_per_account ON cd_keys(user_id, title)
    WHERE user_id IS NOT NULL AND revoked_at IS NULL;

CREATE TABLE cd_key_activations (
    id         bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    key_id     bigint NOT NULL REFERENCES cd_keys(id) ON DELETE CASCADE,
    nwpssk     text NOT NULL,
    nwact      text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now(),
    UNIQUE (key_id, nwpssk)
);

-- ===========================================================================
-- Billing (apps/novaworld_common/billing/billing_repository.h)
-- ===========================================================================
--
-- The monthly support tiers (Stripe subscriptions that support the project and
-- set an account's hosting entitlement), as the service last read them from
-- Stripe, beside an admin's comp grants. Stripe is the record; these tables are
-- its cache, which the entitlement reads whether or not billing is configured,
-- so a deploy that breaks billing never stops a payer's servers.

-- The service's own facts, one row per key. instance_id names this database to
-- Stripe: every customer and subscription the service makes carries it in its
-- metadata (opennova_instance), so a reset, or a developer's local database on
-- a shared Stripe test account, never adopts another instance's subscription.
-- The service mints it at its first boot on the database
-- (billing::instance_id, 32 hex digits from the OS CSPRNG).
CREATE TABLE service_meta (
    key   text PRIMARY KEY,
    value text NOT NULL
);

-- An account's Stripe customer: one per account, made at its first checkout
-- (or relinked by an admin), in the key's mode (livemode).
-- open_checkout_session is the Checkout session last made for it ('' for
-- none), which the next checkout expires first. synced_at is the worker's last
-- full read of the customer.
CREATE TABLE billing_customers (
    user_id               bigint PRIMARY KEY REFERENCES players(id) ON DELETE CASCADE,
    stripe_customer_id    text NOT NULL UNIQUE,
    livemode              boolean NOT NULL,
    open_checkout_session text NOT NULL DEFAULT '',
    created_at            timestamptz NOT NULL DEFAULT now(),
    synced_at             timestamptz
);

-- Every subscription of a customer the service knows, as Stripe last answered
-- it. item is what its price sells: a support tier ('tier', with its tier
-- 1..3), the own-machine add-on ('own_machine', quantity = how many machines),
-- a static address ('static_address', quantity 1: each address its own
-- subscription; apps/novaworld_common/hosted/model/address_repository.h), or
-- none of them ('unknown': a price the configuration no longer names, which
-- grants nothing). status is Stripe's (active, trialing, past_due, canceled,
-- unpaid, incomplete, incomplete_expired, paused); status_since is when the
-- service first read the current one. user_id goes NULL when the account goes,
-- and the subscription row stays (an admin sees it, Stripe still bills it).
CREATE TABLE billing_subscriptions (
    stripe_subscription_id text PRIMARY KEY,
    stripe_customer_id     text NOT NULL,
    user_id                bigint REFERENCES players(id) ON DELETE SET NULL,
    livemode               boolean NOT NULL,
    status                 text NOT NULL,
    item                   text NOT NULL
        CHECK (item IN ('tier', 'own_machine', 'static_address', 'unknown')),
    price_id               text NOT NULL,
    tier                   bigint NOT NULL DEFAULT 0 CHECK (tier BETWEEN 0 AND 3),
    quantity               bigint NOT NULL DEFAULT 1,
    current_period_end     timestamptz,
    cancel_at_period_end   boolean NOT NULL DEFAULT false,
    status_since           timestamptz NOT NULL DEFAULT now(),
    updated_at             timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX idx_billing_subscriptions_user ON billing_subscriptions(user_id);
CREATE INDEX idx_billing_subscriptions_customer ON billing_subscriptions(stripe_customer_id);

-- Every webhook event Stripe delivered (its id once: a redelivery is a no-op),
-- with the customer it names. The worker syncs each customer with an event
-- not yet processed (processed_at NULL), reading the customer and its
-- subscriptions back from Stripe (an event's body is never trusted), then
-- marks its events with the outcome. Processed events go after 30 days.
CREATE TABLE billing_events (
    event_id       text PRIMARY KEY,
    type           text NOT NULL,
    livemode       boolean NOT NULL,
    customer_id    text NOT NULL DEFAULT '',
    stripe_created timestamptz,
    received_at    timestamptz NOT NULL DEFAULT now(),
    processed_at   timestamptz,
    outcome        text NOT NULL DEFAULT ''
);
CREATE INDEX idx_billing_events_pending ON billing_events(received_at) WHERE processed_at IS NULL;

-- The billing sync's state (apps/novaworld_common/billing/billing_sync.h),
-- one row, written by the billing jobs wherever they run and read by the
-- routes and the heartbeat: the configured prices as Stripe last answered
-- them (prices_config names the price lists they were checked under, so a
-- changed configuration reads unchecked until it is checked again), the last
-- full reconcile and what it found, and when the billing sync first ran. The
-- lists are JSON text.
CREATE TABLE billing_sync_state (
    id                     bigint PRIMARY KEY CHECK (id = 1),
    prices_config          text NOT NULL DEFAULT '',
    prices_checked         boolean NOT NULL DEFAULT false,
    prices_ok              boolean NOT NULL DEFAULT false,
    prices_error           text NOT NULL DEFAULT '',
    prices                 text NOT NULL DEFAULT '{}',
    price_check_after_s    bigint NOT NULL DEFAULT 0,
    last_reconcile_s       bigint NOT NULL DEFAULT 0,
    last_reconcile_error   text NOT NULL DEFAULT '',
    orphans                text NOT NULL DEFAULT '[]',
    duplicates             text NOT NULL DEFAULT '[]',
    unknown_prices         text NOT NULL DEFAULT '[]',
    started_s              bigint NOT NULL DEFAULT 0
);
INSERT INTO billing_sync_state (id) VALUES (1);

-- A customer Stripe could not be asked about waits before the billing sync
-- asks again: until next_s (Unix seconds), the wait doubling (delay_s) up to
-- its cap. Gone once the customer syncs.
CREATE TABLE billing_sync_backoff (
    stripe_customer_id text PRIMARY KEY,
    next_s             bigint NOT NULL,
    delay_s            bigint NOT NULL
);

-- An admin's grant of shared servers ('servers') or dedicated machines
-- ('own_machine') to an account, outside any Stripe subscription
-- (apps/novaworld_common/billing/entitlement.h): the entitlement's shared
-- servers are the larger of the servers granted and the support tier's, and
-- each dedicated machine granted adds one server on a machine of its own. One
-- row per account and item; no row is 0.
CREATE TABLE comp_grants (
    user_id    bigint NOT NULL REFERENCES players(id) ON DELETE CASCADE,
    item       text NOT NULL CHECK (item IN ('servers', 'own_machine')),
    quantity   bigint NOT NULL CHECK (quantity BETWEEN 1 AND 16),
    updated_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, item)
);

-- A checkout's hold on the item it sells (billing_repository.h "A checkout's
-- hold"): written with the judgement that the fleet has room for it
-- (hosted/model/sold_capacity.h), under the hosting-start lock, before Stripe
-- makes the session (checkout_session, '' until then), and counted as a
-- granting subscription of the item until the sync writes the subscription it
-- made (its opennova_reservation metadata names the row), the session expires,
-- the account's next checkout settles it, or expires_at passes.
CREATE TABLE billing_checkout_reservations (
    id               bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    user_id          bigint NOT NULL REFERENCES players(id) ON DELETE CASCADE,
    item             text NOT NULL CHECK (item IN ('tier', 'own_machine', 'static_address')),
    tier             bigint NOT NULL DEFAULT 0 CHECK (tier BETWEEN 0 AND 3),
    checkout_session text NOT NULL DEFAULT '',
    created_at       timestamptz NOT NULL DEFAULT now(),
    expires_at       timestamptz NOT NULL
);
CREATE INDEX idx_billing_checkout_reservations_user ON billing_checkout_reservations(user_id);
CREATE INDEX idx_billing_checkout_reservations_session
    ON billing_checkout_reservations(checkout_session) WHERE checkout_session <> '';

-- ===========================================================================
-- The games as data sets, their releases and their projects (ADR 0064, ADR
-- 0069; apps/novaworld_common/data_sets/data_set_repository.h,
-- content_repository.h)
-- ===========================================================================

-- A game: one data set, a base game or one expansion, its hosted servers' files
-- built by the data program into the deployment's object store as immutable
-- trees from its published release; each hosted server pinned to the trees it
-- started on, updated when its owner says. kind 'base' speaks a title (the
-- gameprofile code opennova-serve's /game takes: 'jo', 'dfx2') and lists in a
-- lobby (game: the games row's slug); kind 'expansion' is one expansion of its
-- parent base (expansion_code: the /exp code and the expansion/<code>/
-- directory, 'jox01'); kind 'update' is a standalone updater product with no
-- game tree, no lobby, no parent and no project (OpenNova's runtime, ADR 0067).
-- current_tree_id is the tree new starts take (the newest ready one).
--
-- Its updater product (ADR 0064 d5, d9): updater_login and command_file
-- (JOTRus, JOPTR.CMD; '' for a data set the update service does not serve), and
-- its name as its packages give it (the release build's: the command file's
-- PRODUCT and the executable its DONE runs, '' for an expansion). retail
-- whether it is a retail game (NovaLogic's install: its SERVER.LST names
-- ftp.novaworld.com, reached through the Switcher, where our own game's names
-- the deployment's update host; its project's revision 1 the stock install).
-- published_release_id is the release the update listener serves and its tree
-- is built from.
--
-- Its project (ADR 0069): an OpenNova Editor project's own files as a linear
-- history of revisions in the content store (content_revisions). key names it
-- in the routes ('JO', 'JOX01'; NULL for a game with no project, OpenNova's
-- runtime, whose releases arrive from CI), and the store names its files by the
-- game's id (content/<id>/blobs/<sha256>, content/<id>/manifests/<sha256>.json;
-- core/object_store.h, base/content/content_manifest.h), so a key's rename is a
-- row update. visibility is who may read it (a public project is anyone's to
-- pull, a private one, a retail game's, an admin's alone), head the newest
-- revision's number (0 before the first push: a push names it as its parent and
-- moves it on in the same statement, content_repository.h commit_revision). An
-- expansion's base game is its parent, whose project a release build exports
-- beside it; a retail game's revision 1 is the stock install imported afresh,
-- what its releases count their removals from (ADR 0067 d6).
CREATE TABLE data_sets (
    id                   bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    display_name         text NOT NULL CHECK (display_name <> ''),
    kind                 text NOT NULL CHECK (kind IN ('base', 'expansion', 'update')),
    parent_id            bigint REFERENCES data_sets(id) ON DELETE RESTRICT,
    expansion_code       text NOT NULL DEFAULT '',
    title                text NOT NULL DEFAULT '',
    game                 text NOT NULL DEFAULT '',
    current_tree_id      bigint,
    created_by           text NOT NULL DEFAULT '',
    created_at           timestamptz NOT NULL DEFAULT now(),
    updated_at           timestamptz NOT NULL DEFAULT now(),
    updater_login        text NOT NULL DEFAULT '',
    command_file         text NOT NULL DEFAULT '',
    retail               boolean NOT NULL DEFAULT false,
    published_release_id bigint,
    key                  text UNIQUE CHECK (key ~ '^[A-Z0-9_]{1,16}$'),
    visibility           text CHECK (visibility IN ('public', 'private')),
    head                 bigint NOT NULL DEFAULT 0 CHECK (head >= 0),
    product_name         text NOT NULL DEFAULT '',
    product_exe          text NOT NULL DEFAULT '',
    CHECK ((kind = 'base') = (parent_id IS NULL AND expansion_code = '' AND title <> '' AND game <> '')),
    CHECK ((kind = 'expansion') = (parent_id IS NOT NULL AND expansion_code <> '' AND title = '' AND game = '')),
    CONSTRAINT data_sets_updater_check CHECK ((updater_login = '') = (command_file = '')),
    -- A standalone updater product: a login and its command file, and nothing a game has.
    CONSTRAINT data_sets_update_check CHECK (kind <> 'update' OR (
        parent_id IS NULL AND expansion_code = '' AND title = '' AND game = '' AND
        updater_login <> '' AND current_tree_id IS NULL)),
    CONSTRAINT data_sets_project_check CHECK ((key IS NULL) = (visibility IS NULL)),
    CONSTRAINT data_sets_update_project_check CHECK (kind <> 'update' OR key IS NULL)
);
CREATE UNIQUE INDEX idx_data_sets_expansion ON data_sets(parent_id, expansion_code) WHERE kind = 'expansion';
CREATE INDEX idx_data_sets_parent ON data_sets(parent_id);
CREATE UNIQUE INDEX idx_data_sets_login ON data_sets(upper(updater_login)) WHERE updater_login <> '';

-- One version of a data set's files in the store (trees/<id>/...), built by the
-- data program from a published release (release.apply). Its life: building
-- (the worker applies the release to an empty tree, measures the files and
-- lists its missions) -> ready; failed with `error`; pruned once no start and
-- no live server needs it (its objects removed when pruned_at is set, by the
-- tree.prune job). manifest holds {"files": [{"name", "size"}]} and missions
-- [{"file", "title"}] once ready (the multiplayer missions its archives list:
-- what the start form offers), version_text the release's version, release_id
-- the release it was built from (NULL once that release is deleted).
CREATE TABLE data_set_trees (
    id               bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    data_set_id      bigint NOT NULL REFERENCES data_sets(id) ON DELETE CASCADE,
    state            text NOT NULL CHECK (state IN ('building', 'ready', 'failed', 'pruned')),
    version_text     text NOT NULL DEFAULT '',
    manifest         jsonb NOT NULL DEFAULT '{}'::jsonb,
    missions         jsonb NOT NULL DEFAULT '[]'::jsonb,
    bytes            bigint NOT NULL DEFAULT 0,
    error            text NOT NULL DEFAULT '',
    created_by       text NOT NULL DEFAULT '',
    created_at       timestamptz NOT NULL DEFAULT now(),
    ready_at         timestamptz,
    pruned_at        timestamptz,
    release_id       bigint
);
CREATE INDEX idx_data_set_trees_data_set ON data_set_trees(data_set_id, id);

-- An update release: a 1SFS package, each section a whole build of the data
-- set whose command file it carries, served to retail UPDATE.EXE by the web
-- program's update listener and built into that data set's tree from nothing
-- (release.apply). A game with a project builds its releases from its
-- revisions; one with none (OpenNova's runtime) has its packages uploaded.
-- The package is updates/<sha256>.sfs once validated (updates/upload-<id>.sfs
-- before): uploading -> validating (the worker parses it and maps each section
-- to a data set by its command file) -> draft | invalid -> published (its
-- sections' data sets serve it to the updater and get a tree built from it) ->
-- retired. data_set_id is the game it is a release of (a built one's from the
-- start, an upload's once its validation maps its base section);
-- package_product the 1SFS trailer's; name the release's label.
--
-- A release built from a revision of its game's project (ADR 0069 d7;
-- data_sets/release_build.h): the data program's release.build job exports the
-- revision (an expansion's beside its base game's revision), packs it whole (a
-- retail game's carrying its removals since the stock revision's export), and
-- hands the package to release.validate as an uploaded one is. building ->
-- validating, or invalid with `error`; build_log what the tools said.
-- content_revision and base_revision are the revisions built, version the
-- package's.
CREATE TABLE releases (
    id               bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    status           text NOT NULL DEFAULT 'uploading'
        CONSTRAINT releases_status_check CHECK (status IN
            ('uploading', 'building', 'validating', 'invalid', 'draft', 'published', 'retired')),
    data_set_id      bigint REFERENCES data_sets(id) ON DELETE SET NULL,
    sha256           text NOT NULL DEFAULT '',
    bytes            bigint NOT NULL DEFAULT 0,
    package_product  text NOT NULL DEFAULT '',
    name             text NOT NULL DEFAULT '',
    error            text NOT NULL DEFAULT '',
    created_by       text NOT NULL DEFAULT '',
    created_at       timestamptz NOT NULL DEFAULT now(),
    published_at     timestamptz,
    retired_at       timestamptz,
    content_revision bigint,
    base_revision    bigint,
    version          text NOT NULL DEFAULT '',
    build_log        text NOT NULL DEFAULT ''
);
CREATE UNIQUE INDEX idx_releases_sha ON releases(sha256) WHERE sha256 <> '';

-- One section of a package (the base product's, then one per PACKAGEBREAK),
-- mapped to the data set whose command_file it carries; version the command
-- file's VERSION; tree_id the tree its publish built (NULL until the
-- release.apply job made it).
CREATE TABLE release_sections (
    release_id   bigint NOT NULL REFERENCES releases(id) ON DELETE CASCADE,
    data_set_id  bigint NOT NULL REFERENCES data_sets(id) ON DELETE CASCADE,
    expansion    text NOT NULL DEFAULT '',
    command_file text NOT NULL,
    version      text NOT NULL DEFAULT '',
    row_count    bigint NOT NULL DEFAULT 0,
    blob_bytes   bigint NOT NULL DEFAULT 0,
    tree_id      bigint REFERENCES data_set_trees(id) ON DELETE SET NULL,
    PRIMARY KEY (release_id, data_set_id)
);

ALTER TABLE data_sets
    ADD CONSTRAINT data_sets_current_tree_fkey
        FOREIGN KEY (current_tree_id) REFERENCES data_set_trees(id) ON DELETE SET NULL,
    ADD CONSTRAINT data_sets_published_release_fkey
        FOREIGN KEY (published_release_id) REFERENCES releases(id) ON DELETE SET NULL;
ALTER TABLE data_set_trees
    ADD CONSTRAINT data_set_trees_release_fkey
        FOREIGN KEY (release_id) REFERENCES releases(id) ON DELETE SET NULL;

-- One revision of a game's project: its number (1, 2, ...; its parent is the
-- one before), the admin who pushed it (author_id, kept as created_by once the
-- account is gone), its message, its manifest's SHA-256 and what the manifest
-- lists.
CREATE TABLE content_revisions (
    data_set_id     bigint NOT NULL REFERENCES data_sets(id) ON DELETE CASCADE,
    number          bigint NOT NULL CHECK (number > 0),
    author_id       bigint REFERENCES players(id) ON DELETE SET NULL,
    created_by      text NOT NULL,
    message         text NOT NULL CHECK (message <> ''),
    manifest_sha256 text NOT NULL CHECK (manifest_sha256 ~ '^[0-9a-f]{64}$'),
    file_count      bigint NOT NULL CHECK (file_count >= 0),
    bytes           bigint NOT NULL CHECK (bytes >= 0),
    created_at      timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (data_set_id, number)
);

-- The files a project's store holds, each once by its SHA-256 (what a push
-- need not upload again, and the project's storage).
CREATE TABLE content_blobs (
    data_set_id bigint NOT NULL REFERENCES data_sets(id) ON DELETE CASCADE,
    sha256      text NOT NULL CHECK (sha256 ~ '^[0-9a-f]{64}$'),
    bytes       bigint NOT NULL CHECK (bytes >= 0),
    created_at  timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (data_set_id, sha256)
);

-- The seeded games, in this order, so each keeps its id across a reset (ids 1
-- to 6; DEPLOY.md "Resetting the service database"): the retail games, the
-- update service serving their updaters (the Switcher sends them to it), their
-- projects private and keyed JO, DFX2 and JOX01; OpenNova's own game (OP: Acid
-- Gambit) and its nifty expansion (OP: Nifty Package), complete packages served
-- at the deployment's update host, their projects public and keyed by their
-- logins; and OpenNova's runtime, a standalone updater product (ADR 0067). DFX2's
-- updater product is the Steam DFX2's UPDATE.WIZ (CMD=DFX2.CMD,
-- PRODUCT_LOGIN=DFX2). None holds a tree or a revision until a release of its
-- is published.
INSERT INTO data_sets (display_name, kind, title, game, updater_login, command_file, retail,
                       key, visibility, product_name, product_exe)
VALUES ('Joint Operations', 'base', 'jo', 'jop_2_consumer', 'JOTRus', 'JOPTR.CMD', true,
        'JO', 'private', 'Joint Operations', 'Jointops.exe'),
       ('Delta Force Xtreme 2', 'base', 'dfx2', 'dfx2_consumer', 'DFX2', 'DFX2.CMD', true,
        'DFX2', 'private', 'Delta Force Xtreme 2', 'dfx2.exe'),
       ('OP: Acid Gambit', 'base', 'jo', 'acidgambit_0_consumer', 'ACIDGAMBIT', 'ACIDGAMBIT.CMD',
        false, 'ACIDGAMBIT', 'public', 'OP: Acid Gambit', 'opennova.exe');
INSERT INTO data_sets (display_name, kind, parent_id, expansion_code, updater_login, command_file,
                       retail, key, visibility, product_name)
SELECT 'Escalation', 'expansion', id, 'jox01', 'JOEus', 'JOEPTR.CMD', true, 'JOX01', 'private',
       'Joint Operations: Escalation'
FROM data_sets WHERE kind = 'base' AND game = 'jop_2_consumer';
INSERT INTO data_sets (display_name, kind, updater_login, command_file)
VALUES ('OpenNova runtime', 'update', 'ONRUNTIME', 'ONRUNTIME.CMD');
INSERT INTO data_sets (display_name, kind, parent_id, expansion_code, updater_login, command_file,
                       key, visibility, product_name)
SELECT 'OP: Nifty Package', 'expansion', id, 'nifty', 'NIFTY', 'NIFTY.CMD', 'NIFTY', 'public',
       'OP: Nifty Package'
FROM data_sets WHERE kind = 'base' AND game = 'acidgambit_0_consumer';

-- ===========================================================================
-- Hosted game servers and their fleet (ADR 0053, ADR 0065, ADR 0066;
-- apps/novaworld_common/hosted/README.md)
-- ===========================================================================

-- A static address (hosted/model/address_repository.h): an own machine's fixed
-- public IPv4 with an A record named `name` in the deployment's play zone, held
-- by its owner for the own-machine servers it starts. Its source is its own
-- subscription (stripe_subscription_id, one address each, until the address is
-- released: a subscription granting still may name another) or an admin's
-- grant ('comp'). allocating -> ready -> lapsed (its grant gone, or an admin's
-- release: lapse_cause) -> releasing -> released, or failed (the cloud refused
-- it: reason) -> allocating again (an admin's retry, or, for a refusal that
-- passes, the fleet's pass at retry_at; retries counts the automatic tries for
-- their backoff). allocation_id and public_ip are the cloud's; dns_name the
-- name its record carries now ('' for none), dns_change_id the zone's last
-- change and dns_state whether a delete waits to be in sync; release_after when
-- its record's TTL has drained. A name is unique for good: a released address
-- keeps it until an admin renames the row.
CREATE TABLE hosted_addresses (
    id                     bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    owner_user_id          bigint REFERENCES players(id) ON DELETE SET NULL,
    name                   text NOT NULL,
    source                 text NOT NULL CHECK (source IN ('subscription', 'comp')),
    stripe_subscription_id text,
    state                  text NOT NULL DEFAULT 'allocating'
                           CHECK (state IN ('allocating', 'ready', 'lapsed', 'releasing', 'released', 'failed')),
    allocation_id          text NOT NULL DEFAULT '',
    public_ip              text NOT NULL DEFAULT '',
    dns_name               text NOT NULL DEFAULT '',
    dns_change_id          text NOT NULL DEFAULT '',
    dns_state              text NOT NULL DEFAULT 'none' CHECK (dns_state IN ('none', 'pending', 'insync')),
    release_after          timestamptz,
    lapse_cause            text NOT NULL DEFAULT '' CHECK (lapse_cause IN ('', 'grant', 'admin')),
    reason                 text NOT NULL DEFAULT '',
    retry_at               timestamptz,
    retries                bigint NOT NULL DEFAULT 0 CHECK (retries >= 0),
    created_at             timestamptz NOT NULL DEFAULT now(),
    updated_at             timestamptz NOT NULL DEFAULT now(),
    ready_at               timestamptz,
    lapsed_at              timestamptz,
    released_at            timestamptz,
    CONSTRAINT hosted_addresses_subscription_check
        CHECK ((source = 'subscription') = (stripe_subscription_id IS NOT NULL))
);
CREATE UNIQUE INDEX idx_hosted_addresses_name ON hosted_addresses(name);
CREATE UNIQUE INDEX idx_hosted_addresses_subscription ON hosted_addresses(stripe_subscription_id)
    WHERE stripe_subscription_id IS NOT NULL AND state <> 'released';
CREATE INDEX idx_hosted_addresses_owner ON hosted_addresses(owner_user_id);

-- The addresses the cloud holds for this deployment that no held row names, as
-- the last fleet.sweep found them (reported, never released by it).
CREATE TABLE hosted_address_orphans (
    allocation_id text PRIMARY KEY,
    public_ip     text NOT NULL,
    address_id    bigint NOT NULL DEFAULT 0,
    seen_at       timestamptz NOT NULL DEFAULT now()
);

-- Hosted game servers (ADR 0053): one row per start an owner asks for, walked
-- through requested -> provisioning -> booting -> listing -> running ->
-- stopping -> stopped | failed by the supervisor
-- (apps/novaworld_fleet/hosted/supervisor.h; a crash restart goes back to
-- provisioning). The owner's settings are columns the backend range-checked
-- (hosted/model/hosted_settings.h); the supervisor renders the server's files
-- from them and never takes a file from the owner. An owner has as many live
-- servers (any state but stopped and failed) as its hosting entitlement allows,
-- and own_machine marks one that runs on a machine of its own (a dedicated
-- machine); those counts and the global cap (ONNET_HOSTED_CAP) are taken in the
-- transaction that inserts a request (hosted::request_server). admin_user /
-- admin_password are the remote-admin login the rendered admin.cfg grants the
-- service alone (a new one at every start). owner_user_id and service_user_id
-- are set NULL when the account goes: the supervisor stops a live server whose
-- owner is gone.
--
-- Its place in the fleet: machine_ref is the machine it runs on and slot its
-- slot there (hosted/protocol/slots.h: slot k is the game's UDP port 32768 + k
-- on a cloud machine, game_port the one the server was rendered with), held
-- while the server is live: two live servers never hold one slot of a machine
-- (idx_hosted_servers_live_slot), and an ended server's row keeps where it last
-- ran. generation counts the server's starts: its container is
-- srv-<id>-g<generation> (hosted/protocol/agent_protocol.h), a new name at
-- every start. machine_id holds that container's name and machine_address the
-- machine's private address, where the service's admin link connects (provider
-- names the MachineProvider). serve_image is the opennova-serve image the
-- server's container runs, pinned at each start (a deploy's new image reaches a
-- server at its next start, never mid-run).
--
-- Its game: data_set_id the base data set it runs and expansion_id the
-- expansion of it its owner picked (NULL for none); it is pinned to the trees
-- its start took (base_tree_id, expansion_tree_id: NULL before its first start)
-- and keeps them through crash restarts and moves until its owner asks for an
-- update (update_requested: the supervisor restarts it on the data sets'
-- current trees, then clears the flag). address_id is the static address an
-- own-machine server asked for (one live server per address).
-- list_on_novalogic is its owner's ask that it be listed on NovaLogic's
-- NovaWorld as well as this service's (hosted_settings.h
-- HostedSettings::list_on_novalogic, ADR 0068): opennova-serve's second listing
-- (--also-master-host), rendered at each start while the service offers it
-- (ONNET_NOVALOGIC_LISTING with the project's NovaLogic account).
CREATE TABLE hosted_servers (
    id                bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    owner_user_id     bigint REFERENCES players(id) ON DELETE SET NULL,
    service_user_id   bigint REFERENCES players(id) ON DELETE SET NULL,
    state             text NOT NULL DEFAULT 'requested'
        CHECK (state IN ('requested', 'provisioning', 'booting', 'listing', 'running',
                         'stopping', 'stopped', 'failed')),
    -- The owner's settings, range-checked (hosted_settings.h).
    game_name         text NOT NULL,
    max_players       bigint NOT NULL,
    game_password     text NOT NULL DEFAULT '',
    time_limit        bigint NOT NULL,
    kill_limit        bigint NOT NULL,
    max_score         bigint NOT NULL,
    respawn           bigint NOT NULL,
    start_delay       bigint NOT NULL,
    team_ff           boolean NOT NULL DEFAULT false,
    missions          text NOT NULL,             -- the rotation, one file name per line
    -- The machine.
    provider          text NOT NULL DEFAULT '',
    machine_id        text NOT NULL DEFAULT '',
    machine_address   text NOT NULL DEFAULT '',
    admin_port        bigint NOT NULL DEFAULT 0,
    admin_user        text NOT NULL DEFAULT '',
    admin_password    text NOT NULL DEFAULT '',
    restarts          bigint NOT NULL DEFAULT 0,
    failure_reason    text NOT NULL DEFAULT '',
    created_at        timestamptz NOT NULL DEFAULT now(),
    updated_at        timestamptz NOT NULL DEFAULT now(),
    running_at        timestamptz,
    ended_at          timestamptz,
    own_machine       boolean NOT NULL DEFAULT false,
    -- Its place in the fleet (machine_ref's foreign key follows fleet_machines).
    machine_ref       bigint,
    slot              bigint CHECK (slot IS NULL OR (slot >= 0 AND slot < 32)),
    game_port         bigint NOT NULL DEFAULT 0 CHECK (game_port BETWEEN 0 AND 65535),
    generation        bigint NOT NULL DEFAULT 0,
    serve_image       text NOT NULL DEFAULT '',
    -- Its game.
    data_set_id       bigint REFERENCES data_sets(id) ON DELETE SET NULL,
    expansion_id      bigint REFERENCES data_sets(id) ON DELETE SET NULL,
    base_tree_id      bigint REFERENCES data_set_trees(id) ON DELETE SET NULL,
    expansion_tree_id bigint REFERENCES data_set_trees(id) ON DELETE SET NULL,
    update_requested  boolean NOT NULL DEFAULT false,
    address_id        bigint REFERENCES hosted_addresses(id) ON DELETE SET NULL,
    list_on_novalogic boolean NOT NULL DEFAULT false
);
CREATE INDEX idx_hosted_servers_owner ON hosted_servers(owner_user_id);
CREATE INDEX idx_hosted_servers_state ON hosted_servers(state);
CREATE INDEX idx_hosted_servers_service_user ON hosted_servers(service_user_id);
CREATE UNIQUE INDEX idx_hosted_servers_live_slot ON hosted_servers(machine_ref, slot)
    WHERE machine_ref IS NOT NULL AND state NOT IN ('stopped', 'failed');
CREATE INDEX idx_hosted_servers_machine_ref ON hosted_servers(machine_ref);
CREATE INDEX idx_hosted_servers_data_set ON hosted_servers(data_set_id);
CREATE INDEX idx_hosted_servers_pins ON hosted_servers(base_tree_id, expansion_tree_id)
    WHERE state NOT IN ('stopped', 'failed');
CREATE UNIQUE INDEX idx_hosted_servers_one_per_address ON hosted_servers(address_id)
    WHERE address_id IS NOT NULL AND state NOT IN ('stopped', 'failed');

-- One row per hosted server state transition, with what caused it.
CREATE TABLE hosted_server_events (
    id         bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    server_id  bigint NOT NULL REFERENCES hosted_servers(id) ON DELETE CASCADE,
    from_state text NOT NULL,
    to_state   text NOT NULL,
    detail     text NOT NULL DEFAULT '',
    at         timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX idx_hosted_server_events_server ON hosted_server_events(server_id, id);

-- A hosted server's remote-admin console (ADR 0053 d5): the service keeps one
-- logged-in session on each running server's retail remote-admin port
-- (apps/novaworld_fleet/hosted/admin_link.h), and its console is this ring, one
-- row per line: a command the link sent for an owner, an admin or the service
-- (actor names which), a reply packet (a PLAYER LIST table is one row), or the
-- link's own status lines. The text is UTF-8 (the server's cp1252 decoded); the
-- admin password never reaches it. Every append keeps a server's newest 500
-- rows (hosted::append_console).
CREATE TABLE hosted_server_console (
    id        bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    server_id bigint NOT NULL REFERENCES hosted_servers(id) ON DELETE CASCADE,
    kind      text NOT NULL CHECK (kind IN ('command', 'reply', 'status')),
    actor     text NOT NULL DEFAULT '' CHECK (actor IN ('', 'owner', 'admin', 'service')),
    text      text NOT NULL,
    at        timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX idx_hosted_server_console_server ON hosted_server_console(server_id, id);

-- The fleet (hosted/model/fleet_repository.h): the machines the service
-- launches, the slot each live server holds on one, and what each machine's
-- agent (opennova-hostd) last reported of itself and its slots.
--
-- A machine: an EC2 virtual machine (provider 'ec2') or a development stand-in
-- (provider 'local'), launched from the launch template of its kind. A shared
-- machine packs up to `slots` servers of any account; an own machine runs one,
-- the server of the dedicated machine it is reserved_for, and ends when that
-- server stops (hosted/model/placement.h). Its lifecycle:
--
--   launching   the row and its one-time bootstrap token are made; the cloud
--               is asked for the machine (cloud_id empty until it answers)
--   booting     the cloud runs it (cloud_id, private_address set); its agent
--               has not reported yet
--   ready       its agent reports (last_report_at); servers land on it
--   draining    its servers move off it, one at a time; nothing new lands
--   ending      the cloud is asked to end it
--   ended       gone (ended_at, end_reason)
--   failed      gone after a fault (a launch the cloud refused, a boot that
--               never reported)
--   lost        its agent stopped reporting (last_report_at older than a
--               minute): the cloud is asked whether it still runs
--
-- The agent's credentials are SHA-256 digests (64 lower-case hex digits),
-- never the values: bootstrap_token_hash is the one-time token the machine's
-- user-data carries, until it is redeemed (then '') or lapses at
-- bootstrap_expires_at; agent_secret_hash the long-lived secret the bootstrap
-- hands the agent for its syncs ('' until then). An ended or failed machine
-- keeps neither. serve_image is the image the machine's agent was copied out
-- of (the bootstrap reply names it); template_version the launch template and
-- its version the cloud launched it from, so a fleet roll can tell an old
-- machine from a new one.
--
-- The machine on the jobs (hosted/model/fleet_jobs.h): the supervisor's pass
-- never calls the cloud; it enqueues machine.launch, machine.describe and
-- machine.terminate and reads their results here. cloud_state is what the last
-- describe (or the sweep's) said of the machine ('' until one did, and again
-- from when it went lost): pending, running, gone or unknown (the cloud did not
-- answer); described_at when. launched_for is the server whose placement
-- launched it; launch_refused a launch the cloud refused that the supervisor
-- has not counted yet (its launch backoff, the heartbeat's launch_failures, the
-- server's event). launch_subnet is the subnet its launch last called the cloud
-- in, written before that call ('' until one did): a launch run again calls
-- there first, under the same client token, so EC2 answers it with the machine
-- the earlier call launched, however late EC2 lists that machine.
--
-- What the supervisor remembers of a live machine between passes, here so a
-- new leader resumes it (supervisor.h): seen_state and seen_since_ms, the state
-- its pass last saw and since when on its clock (Unix milliseconds);
-- idle_since_ms, since when a shared machine has held no server (0: it holds
-- one); drain_moving, the server its drain moved last (0: none); was_draining,
-- a lost machine that was draining; describe_asked_ms, when the pass last asked
-- for a describe; lost_held_since_ms, since when the mass-lost brake holds this
-- lost machine from its end (0: not held).
--
-- The envelope every sync opens with (agent_protocol.h AgentEnvelope),
-- recorded whether or not its protocol is the service's: agent_protocol the
-- protocol the agent speaks (NULL before its first sync), agent_image the image
-- its ECS task runs ('' off ECS), and protocol_mismatch_since when its syncs
-- began to speak another protocol than the service's (NULL while they match). A
-- mismatched machine is held: heard, acted on in nothing, and ended only once
-- the mismatch has lasted 10 minutes (apps/novaworld_fleet/hosted/agent_route.h),
-- so a hostd rolled before the fleet program, or after it, keeps its servers
-- running. What the agent last said of the machine: its image (an AMI id, '' off
-- EC2), the agent's own log stream ('' off ECS), and its use as the last report
-- read it (stats_at; all null before one): the CPU busy of the whole machine
-- (0..100), its memory, the trees' filesystem, and every interface's bytes but
-- loopback's since the machine booted. address_id is the static address the
-- machine was launched to carry.
CREATE TABLE fleet_machines (
    id                      bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    provider                text NOT NULL CHECK (provider <> ''),
    kind                    text NOT NULL CHECK (kind IN ('shared', 'own')),
    state                   text NOT NULL DEFAULT 'launching'
        CHECK (state IN ('launching', 'booting', 'ready', 'draining', 'ending', 'ended',
                         'failed', 'lost')),
    cloud_id                text NOT NULL DEFAULT '',
    instance_type           text NOT NULL DEFAULT '',
    slots                   bigint NOT NULL CHECK (slots BETWEEN 1 AND 32),
    private_address         text NOT NULL DEFAULT '',
    public_address          text NOT NULL DEFAULT '',
    bootstrap_token_hash    text NOT NULL DEFAULT '',
    bootstrap_expires_at    timestamptz,
    agent_secret_hash       text NOT NULL DEFAULT '',
    agent_version           text NOT NULL DEFAULT '',
    template_version        text NOT NULL DEFAULT '',
    serve_image             text NOT NULL DEFAULT '',
    reserved_for            bigint REFERENCES hosted_servers(id) ON DELETE SET NULL,
    last_report_at          timestamptz,
    launched_at             timestamptz NOT NULL DEFAULT now(),
    ready_at                timestamptz,
    ended_at                timestamptz,
    end_reason              text NOT NULL DEFAULT '',
    -- On the jobs.
    cloud_state             text NOT NULL DEFAULT ''
        CHECK (cloud_state IN ('', 'pending', 'running', 'gone', 'unknown')),
    described_at            timestamptz,
    launched_for            bigint REFERENCES hosted_servers(id) ON DELETE SET NULL,
    launch_refused          boolean NOT NULL DEFAULT false,
    launch_subnet           text NOT NULL DEFAULT '',
    -- The supervisor's memory.
    seen_state              text NOT NULL DEFAULT '',
    seen_since_ms           bigint NOT NULL DEFAULT 0,
    idle_since_ms           bigint NOT NULL DEFAULT 0,
    drain_moving            bigint NOT NULL DEFAULT 0,
    was_draining            boolean NOT NULL DEFAULT false,
    describe_asked_ms       bigint NOT NULL DEFAULT 0,
    -- The agent's envelope.
    agent_protocol          bigint,
    agent_image             text NOT NULL DEFAULT '',
    protocol_mismatch_since timestamptz,
    lost_held_since_ms      bigint NOT NULL DEFAULT 0,
    -- What the agent says of the machine.
    image_id                text NOT NULL DEFAULT '',
    agent_log_stream        text NOT NULL DEFAULT '',
    cpu_percent             double precision,
    memory_used_bytes       bigint,
    memory_total_bytes      bigint,
    disk_free_bytes         bigint,
    disk_total_bytes        bigint,
    net_in_bytes            bigint,
    net_out_bytes           bigint,
    stats_at                timestamptz,
    address_id              bigint REFERENCES hosted_addresses(id) ON DELETE SET NULL
);
CREATE INDEX idx_fleet_machines_state ON fleet_machines(state);
-- One row per cloud machine: a listing's machine maps back to its row.
CREATE UNIQUE INDEX idx_fleet_machines_cloud_id ON fleet_machines(provider, cloud_id)
    WHERE cloud_id <> '';
CREATE INDEX idx_fleet_machines_launch_refused ON fleet_machines(id) WHERE launch_refused;

ALTER TABLE hosted_servers
    ADD CONSTRAINT hosted_servers_machine_ref_fkey
        FOREIGN KEY (machine_ref) REFERENCES fleet_machines(id) ON DELETE SET NULL;

-- Every move of a machine, written in the same statement as the move
-- (fleet_repository.h): from_state is null for the row's making, and a row
-- whose to_state is its from_state notes the agent's new log stream ("log
-- stream <name>"). Kept with the machine's row (14 days past its end).
CREATE TABLE fleet_machine_events (
    id         bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    machine_id bigint NOT NULL REFERENCES fleet_machines(id) ON DELETE CASCADE,
    from_state text,
    to_state   text NOT NULL,
    detail     text NOT NULL DEFAULT '',
    at         timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX idx_fleet_machine_events_machine ON fleet_machine_events(machine_id, id);
CREATE INDEX idx_fleet_machine_events_at ON fleet_machine_events(at);

-- Every tree a machine's agent holds, as its last report said (each report
-- replaces the machine's rows): fetching (a failure with a try left among
-- them), ready, or failed for good; its bytes on disk (the fetch's so far), the
-- fetches made and the last failure's reason.
CREATE TABLE fleet_machine_trees (
    machine_id  bigint NOT NULL REFERENCES fleet_machines(id) ON DELETE CASCADE,
    tree_id     bigint NOT NULL,
    state       text NOT NULL CHECK (state IN ('fetching', 'ready', 'failed')),
    bytes       bigint NOT NULL DEFAULT 0 CHECK (bytes >= 0),
    tries       bigint NOT NULL DEFAULT 0 CHECK (tries >= 0),
    error       text NOT NULL DEFAULT '',
    reported_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (machine_id, tree_id)
);

-- What a machine's agent last reported of each slot it runs a container on
-- (POST /hosted/machines/<id>/sync, agent_route.h): the container, its state
-- ('starting', 'running', 'exited'), how it exited, when it started, its CPU
-- (percent of one CPU) and memory, and the slot's egress (every byte the
-- slot's user sent since the machine booted: the stats take the differences).
-- Each sync replaces the machine's rows: a slot it no longer reports goes.
-- reason is why a container that never started did not (agent_protocol.h
-- ContainerReport `reason`: its tree could not be fetched), '' for every other
-- report: the supervisor's failure event says so. waiting is what a starting
-- container waits for ('image', 'trees' or ''), net_in_bytes the bytes that
-- reached the slot's ports since the machine booted, and the tick columns its
-- running server's last minute of ticks (null without a fresh one).
--
-- The second listing columns are its running server's listing on NovaLogic's
-- NovaWorld, as its agent last read it (agent_protocol.h SecondListingStatus,
-- opennova-serve's --second-listing-file), for an owner who asked
-- (hosted_servers.list_on_novalogic, ADR 0068), so its page shows whether and
-- why it is listed there: its state (null without a report: none asked, or an
-- agent or server from before it), the try under way or the next of the run's
-- most and a retry's time, the reason a retry waits or the listing ended with
-- its NWEC tag ('' for none), and the PlayerList slots of the players that
-- service sent, comma-separated ('' for none).
CREATE TABLE fleet_slot_reports (
    machine_id                 bigint NOT NULL REFERENCES fleet_machines(id) ON DELETE CASCADE,
    slot                       bigint NOT NULL CHECK (slot >= 0 AND slot < 32),
    container                  text NOT NULL,
    state                      text NOT NULL CHECK (state IN ('starting', 'running', 'exited')),
    exit_code                  bigint,
    oom                        boolean NOT NULL DEFAULT false,
    started_at                 timestamptz,
    cpu_percent                double precision NOT NULL DEFAULT 0,
    memory_bytes               bigint NOT NULL DEFAULT 0,
    egress_bytes               bigint NOT NULL DEFAULT 0,
    reported_at                timestamptz NOT NULL DEFAULT now(),
    reason                     text NOT NULL DEFAULT '',
    waiting                    text NOT NULL DEFAULT '' CHECK (waiting IN ('', 'image', 'trees')),
    net_in_bytes               bigint NOT NULL DEFAULT 0,
    tick_p50_ms                double precision,
    tick_p99_ms                double precision,
    tick_max_ms                double precision,
    late_ticks                 bigint,
    second_listing_state       text
        CHECK (second_listing_state IN ('off', 'trying', 'listed', 'retrying', 'not_listed')),
    second_listing_try         bigint,
    second_listing_tries       bigint,
    second_listing_next_try_at timestamptz,
    second_listing_reason      text NOT NULL DEFAULT '',
    second_listing_tag         text NOT NULL DEFAULT '',
    second_listing_slots       text NOT NULL DEFAULT '',
    PRIMARY KEY (machine_id, slot)
);

-- After a launch of a kind the cloud refused (or a machine of it that never
-- booted), no launch of that kind before not_before_ms, on the supervisor's
-- clock (Unix milliseconds). failures is the kind's failed launches in a row,
-- counted once per backoff (a failure that lands while one holds came from a
-- launch made before it began, so it changes nothing): the n-th holds launches
-- of the kind for the base backoff doubled n - 1 times, to a cap; a machine of
-- the kind that boots (its agent's first sync) sets the count back to 0 and
-- ends the backoff (supervisor.h).
CREATE TABLE fleet_launch_backoff (
    kind          text PRIMARY KEY CHECK (kind IN ('shared', 'own')),
    not_before_ms bigint NOT NULL,
    failures      bigint NOT NULL DEFAULT 0 CHECK (failures >= 0)
);

-- Each run of a hosted server: a hosted_servers row is one owner's request, a
-- generation one of its starts (its container srv-<id>-g<generation>, its log
-- stream <id>/g<generation>). Made when its slot is assigned (placed_at, the
-- machine and the slot), its image, trees and log stream written when the start
-- renders, running_at when its agent first reports it running, and ended with
-- why (hosted/model/hosted_repository.h StartCause): the cause (crash: the
-- server exited or never came up, its exit code, OOM flag and its exit line or
-- why it never started kept; timeout: it did not start or list in time; fleet:
-- its machine's or NovaWorld's cause, a move; owner_stop and admin_stop: its
-- owner's and an admin's stop, the cause set when the stop is asked; stop: the
-- service's own (its owner's account deleted or no longer active); update: the
-- owner's update to the current data; limits: its owner's entitlement no longer
-- covers it; no_data: its data set has none; link_lost: its admin link stopped
-- answering; data_unavailable: its machine could not fetch its data, no restart
-- counted). A limits end also keeps the limit its server was past (StopLimit):
-- its owner's servers on shared machines ('shared_servers'), or its dedicated
-- machines with no shared server free to move it to ('no_shared_server') or no
-- room on the shared machines ('no_shared_room'), and how many its owner had
-- then (limit_count). The restarts the stats count are its crash and timeout
-- ends. machine_id stays a plain id: the run outlives its machine's row.
CREATE TABLE hosted_server_starts (
    server_id         bigint NOT NULL REFERENCES hosted_servers(id) ON DELETE CASCADE,
    generation        bigint NOT NULL,
    machine_id        bigint NOT NULL,
    slot              bigint NOT NULL CHECK (slot >= 0 AND slot < 32),
    serve_image       text NOT NULL DEFAULT '',
    base_tree_id      bigint,
    expansion_tree_id bigint,
    placed_at         timestamptz NOT NULL DEFAULT now(),
    running_at        timestamptz,
    ended_at          timestamptz,
    exit_code         bigint,
    oom               boolean NOT NULL DEFAULT false,
    reason            text NOT NULL DEFAULT '',
    cause             text CHECK (cause IN ('crash', 'fleet', 'stop', 'owner_stop', 'admin_stop',
                                            'update', 'limits', 'no_data', 'timeout',
                                            'link_lost', 'data_unavailable')),
    log_stream        text NOT NULL DEFAULT '',
    limit_kind        text CHECK (limit_kind IN ('shared_servers', 'no_shared_server',
                                                 'no_shared_room')),
    limit_count       bigint CHECK (limit_count >= 0),
    CONSTRAINT hosted_server_starts_limit_check CHECK ((limit_kind IS NULL) = (limit_count IS NULL)),
    PRIMARY KEY (server_id, generation)
);
CREATE INDEX idx_hosted_server_starts_ended ON hosted_server_starts(ended_at);
CREATE INDEX idx_hosted_server_starts_open ON hosted_server_starts(server_id)
    WHERE ended_at IS NULL;

-- The hosted servers' stats (apps/novaworld_fleet/hosted/stats_sampler.h,
-- hosted/model/stats_repository.h): one sample of each live server a minute,
-- rolled up by the hour, and each machine's use by the minute, the hour and its
-- egress by the calendar month. Counts alone: no player's name is stored
-- (unique players are counted in the service's memory, an hour at a time).

-- A server's minute: `at` the minute it was taken (UTC, seconds 0), its state
-- then, its listing's players, slots and mission (null while it is not
-- listed), its admin link's scene ('game', 'menus' or 'unknown'; null with no
-- link) and whether the link was up, its container's CPU (percent of the
-- slot's CPUs, 0..100) and memory as its machine's agent last reported them
-- (null with no report), the bytes its slot sent this minute (the agent's
-- per-slot counter's difference; a counter that started over counts from 0),
-- and whether this minute saw a new mission start in the game scene (the
-- rotation's missions, counted from the changes the samples see: an
-- approximation). Its slot's network as counters (bytes in and out since the
-- server's first sample on the slot: a reader takes the differences, and a
-- count below the last starts a new run of them) and its last minute of ticks.
-- Kept 48 hours.
CREATE TABLE hosted_server_samples (
    server_id       bigint NOT NULL REFERENCES hosted_servers(id) ON DELETE CASCADE,
    at              timestamptz NOT NULL,
    state           text NOT NULL,
    players         bigint,
    max_players     bigint,
    mission         text,
    scene           text,
    link_up         boolean NOT NULL DEFAULT false,
    cpu_percent     double precision,
    memory_bytes    bigint,
    egress_bytes    bigint NOT NULL DEFAULT 0 CHECK (egress_bytes >= 0),
    mission_started boolean NOT NULL DEFAULT false,
    net_in_bytes    bigint,
    net_out_bytes   bigint,
    tick_p50_ms     double precision,
    tick_p99_ms     double precision,
    tick_max_ms     double precision,
    late_ticks      bigint,
    PRIMARY KEY (server_id, at)
);
CREATE INDEX idx_hosted_server_samples_at ON hosted_server_samples(at);

-- A server's hour (UTC), from its samples: the minutes it was live and
-- running, its player-minutes, its peak players and slots, the distinct
-- players its listing's roster named (counted, never stored), the missions
-- started, the bytes sent and the mean CPU, the peak memory, the bytes in and
-- out, the worst minute's p99 and the late ticks. Rewritten from the samples
-- every minute while the hour runs. Kept 400 days.
CREATE TABLE hosted_server_hourly (
    server_id         bigint NOT NULL REFERENCES hosted_servers(id) ON DELETE CASCADE,
    hour              timestamptz NOT NULL,
    live_minutes      bigint NOT NULL DEFAULT 0,
    running_minutes   bigint NOT NULL DEFAULT 0,
    player_minutes    bigint NOT NULL DEFAULT 0,
    peak_players      bigint NOT NULL DEFAULT 0,
    max_players       bigint NOT NULL DEFAULT 0,
    unique_players    bigint NOT NULL DEFAULT 0,
    missions          bigint NOT NULL DEFAULT 0,
    egress_bytes      bigint NOT NULL DEFAULT 0,
    avg_cpu           double precision,
    peak_memory_bytes bigint,
    net_in_bytes      bigint NOT NULL DEFAULT 0,
    net_out_bytes     bigint NOT NULL DEFAULT 0,
    tick_p99_max_ms   double precision,
    late_ticks        bigint NOT NULL DEFAULT 0,
    PRIMARY KEY (server_id, hour)
);
CREATE INDEX idx_hosted_server_hourly_hour ON hosted_server_hourly(hour);

-- A machine's minute, from its agent's last report (its fleet_machines
-- columns): its use and how many live servers it held; the network as the
-- agent's counters (since the machine booted). Kept 48 hours. Its hour, rolled
-- up from the minutes by stats.prune: the CPU's mean and peak, the memory's
-- peak, the trees' filesystem's least room, the bytes in and out. Kept 400
-- days. machine_id is a plain id: the history outlives the machine's row.
CREATE TABLE fleet_machine_samples (
    machine_id         bigint NOT NULL,
    at                 timestamptz NOT NULL,
    cpu_percent        double precision,
    memory_used_bytes  bigint,
    memory_total_bytes bigint,
    disk_free_bytes    bigint,
    net_in_bytes       bigint,
    net_out_bytes      bigint,
    servers            bigint NOT NULL DEFAULT 0,
    PRIMARY KEY (machine_id, at)
);
CREATE INDEX idx_fleet_machine_samples_at ON fleet_machine_samples(at);
CREATE TABLE fleet_machine_hourly (
    machine_id          bigint NOT NULL,
    hour                timestamptz NOT NULL,
    cpu_avg             double precision,
    cpu_max             double precision,
    memory_peak_bytes   bigint,
    disk_free_min_bytes bigint,
    net_in_bytes        bigint NOT NULL DEFAULT 0,
    net_out_bytes       bigint NOT NULL DEFAULT 0,
    PRIMARY KEY (machine_id, hour)
);
CREATE INDEX idx_fleet_machine_hourly_hour ON fleet_machine_hourly(hour);

-- A machine's egress by calendar month (UTC): every byte its slots sent, as
-- the samples count it (the admin's fleet view). It outlives the machine's row
-- (14 days past its end) to its own 13 months: machine_id is a plain id.
CREATE TABLE fleet_machine_egress (
    machine_id   bigint NOT NULL,
    month        date NOT NULL,
    egress_bytes bigint NOT NULL DEFAULT 0 CHECK (egress_bytes >= 0),
    PRIMARY KEY (machine_id, month)
);

-- The fleet program's state in rows (apps/novaworld_fleet/, ADR 0065): what
-- the fleet program keeps and shares with the routes, so the fleet, the web
-- and the workers share it through the database alone.

-- The fleet program's leadership (apps/novaworld_fleet/election.h): one
-- process leads, holding a session advisory lock on a connection of its own;
-- each new leader moves the generation on, and a leader whose generation is no
-- longer the row's runs no pass (a stale one fenced off until it exits).
CREATE TABLE fleet_leader (
    id         bigint PRIMARY KEY CHECK (id = 1),
    generation bigint NOT NULL DEFAULT 0,
    holder     text NOT NULL DEFAULT '',
    elected_at timestamptz
);
INSERT INTO fleet_leader (id) VALUES (1);

-- The owners' limits' hold (hosted/model/limits_hold.h), one row: whether it
-- holds and why, the owners it waits on a reconcile for and the reconciles
-- counted when it took them, the last pass's counts for the admin's view, the
-- clean reconciles with Stripe so far (the billing sync's), and an admin's
-- override for the next pass.
CREATE TABLE hosted_limits_hold (
    id               bigint PRIMARY KEY CHECK (id = 1),
    held             boolean NOT NULL DEFAULT false,
    reason           text NOT NULL DEFAULT '',
    owners           bigint[] NOT NULL DEFAULT '{}',
    taken_reconciles bigint NOT NULL DEFAULT 0,
    last_owners      bigint NOT NULL DEFAULT 0,
    servers          bigint NOT NULL DEFAULT 0,
    reconciles       bigint NOT NULL DEFAULT 0,
    force_next       boolean NOT NULL DEFAULT false
);
INSERT INTO hosted_limits_hold (id) VALUES (1);

-- Each running hosted server's admin link as the fleet program last saw it
-- (admin_link.h AdminLinkStatus): its state, the server's scene at the last
-- poll, when that poll answered and since when the session is up, why it last
-- failed; seen_at moves while the fleet program keeps the link, so a row it
-- stopped refreshing (no leader) reads as no link. The web routes read these
-- rows, never a link.
CREATE TABLE hosted_admin_links (
    server_id    bigint PRIMARY KEY REFERENCES hosted_servers(id) ON DELETE CASCADE,
    state        text NOT NULL CHECK (state IN ('connecting', 'up', 'down', 'stopped')),
    game_state   text NOT NULL DEFAULT '',
    last_poll_at timestamptz,
    up_since_at  timestamptz,
    error        text NOT NULL DEFAULT '',
    seen_at      timestamptz NOT NULL DEFAULT now()
);

-- The console routes' commands (hosted/model/console_queue.h): a web route
-- writes one, the fleet program runs it on the server's admin link and writes
-- what came back (its outcome, whether it was sent, each reply packet in the
-- server's code page), and the route reads it and deletes it. A command not run
-- by its deadline is not run; a row left past it (its route gone) is deleted by
-- the fleet program. claimed_generation is the leadership's generation of the
-- leader that claimed it (fleet_leader): one still running under an earlier
-- generation lost its leader, and the next leader answers it link_down.
CREATE TABLE hosted_console_commands (
    id                 bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    server_id          bigint NOT NULL REFERENCES hosted_servers(id) ON DELETE CASCADE,
    line               text NOT NULL CHECK (line <> ''),
    actor              text NOT NULL,
    record             boolean NOT NULL DEFAULT false,
    state              text NOT NULL DEFAULT 'queued'
        CHECK (state IN ('queued', 'running', 'done', 'cancelled')),
    outcome            text NOT NULL DEFAULT '',
    sent               boolean NOT NULL DEFAULT false,
    claimed_generation bigint NOT NULL DEFAULT 0,
    deadline_at        timestamptz NOT NULL,
    created_at         timestamptz NOT NULL DEFAULT now(),
    finished_at        timestamptz
);
CREATE INDEX idx_hosted_console_commands_queued ON hosted_console_commands(id) WHERE state = 'queued';
CREATE INDEX idx_hosted_console_commands_deadline ON hosted_console_commands(deadline_at);

CREATE TABLE hosted_console_replies (
    command_id bigint NOT NULL REFERENCES hosted_console_commands(id) ON DELETE CASCADE,
    n          bigint NOT NULL,
    text       text NOT NULL,
    PRIMARY KEY (command_id, n)
);

-- ===========================================================================
-- Background jobs and the programs' use
-- ===========================================================================

-- The service's background jobs (apps/novaworld_common/jobs/job_queue.h): one
-- queue in the database for the work no request or pass waits on (the fleet's
-- cloud calls, the billing sync, the outbound mail, the data program's builds,
-- the prunes), so a job is written in the same transaction as the change that
-- causes it and survives a restart of whatever wrote it. A job: what to run
-- (kind, payload), its state, when it may run next, how often it ran, the
-- worker holding it and until when, and why it last failed.
--
--   queued    waiting for run_at; a worker claims it (FOR UPDATE SKIP LOCKED)
--   running   a worker holds it until lease_until, which it extends while it
--             runs; an expired lease (its worker went away) puts it back to
--             queued, or failed once attempts reaches max_attempts
--   done      it ran
--   failed    it failed max_attempts times, or failed for good
--
-- secret holds what the payload must not keep (a machine's one-time bootstrap
-- token, a mail's body with its reset link): set at enqueue, cleared when the
-- job ends. dedupe_key is unique among the queued and running jobs (a machine
-- has at most one describe waiting); schedule_key is unique for good, the
-- periodic jobs' kind and time bucket. Finished jobs are pruned 14 days after
-- they end (the jobs.prune job).
CREATE TABLE jobs (
    id           bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    kind         text NOT NULL CHECK (kind <> ''),
    payload      jsonb NOT NULL DEFAULT '{}',
    secret       text,
    state        text NOT NULL DEFAULT 'queued'
        CHECK (state IN ('queued', 'running', 'done', 'failed')),
    run_at       timestamptz NOT NULL DEFAULT now(),
    attempts     bigint NOT NULL DEFAULT 0 CHECK (attempts >= 0),
    max_attempts bigint NOT NULL DEFAULT 5 CHECK (max_attempts >= 1),
    lease_until  timestamptz,
    worker_id    text,
    last_error   text NOT NULL DEFAULT '',
    dedupe_key   text CHECK (dedupe_key IS NULL OR dedupe_key <> ''),
    schedule_key text UNIQUE CHECK (schedule_key IS NULL OR schedule_key <> ''),
    created_at   timestamptz NOT NULL DEFAULT now(),
    started_at   timestamptz,
    finished_at  timestamptz
);
CREATE UNIQUE INDEX idx_jobs_dedupe ON jobs(dedupe_key)
    WHERE dedupe_key IS NOT NULL AND state IN ('queued', 'running');
CREATE INDEX idx_jobs_queued ON jobs(run_at, id) WHERE state = 'queued';
CREATE INDEX idx_jobs_running ON jobs(lease_until) WHERE state = 'running';
CREATE INDEX idx_jobs_finished ON jobs(finished_at) WHERE state IN ('done', 'failed');
CREATE INDEX idx_jobs_kind_state ON jobs(kind, state);

-- What each program's tasks use (apps/novaworld_common/program/program_usage.h):
-- every program's task samples itself once a minute and writes its own row,
-- kept 48 hours, rolled up by the hour (the worker's stats.prune) and kept 400
-- days. Keyed by the program and its task (an ECS task's id; a local
-- container's hostname), with no foreign key: a task's history outlives the
-- task. The admin's usage view reads them (apps/novaworld_web/infra/usage_routes.h).
--
-- A task's minute: `at` the minute it was taken (UTC, seconds 0); its CPU over
-- the minute before, in percent of its CPU limit (0..100; null for a task's
-- first minute, which has no reading before it), that limit in CPUs; its memory
-- as `docker stats` counts it and the most it may use; and its network
-- namespace's bytes in and out since it came up (cumulative counters: a reader
-- takes the rate between two minutes, and a smaller count is one that started
-- over). Each null when its file did not read.
CREATE TABLE program_samples (
    program            text NOT NULL,
    task_id            text NOT NULL,
    at                 timestamptz NOT NULL,
    cpu_percent        real,
    cpu_limit          real,
    memory_used_bytes  bigint,
    memory_limit_bytes bigint,
    net_in_bytes       bigint,
    net_out_bytes      bigint,
    PRIMARY KEY (program, task_id, at)
);
CREATE INDEX idx_program_samples_at ON program_samples(at);

-- A task's hour (UTC), from its minutes: their mean and peak CPU, its peak
-- memory and the bytes it took in and sent in that hour (a counter that started
-- over counts from 0). Rewritten while the hour's minutes are kept.
CREATE TABLE program_hourly (
    program           text NOT NULL,
    task_id           text NOT NULL,
    hour              timestamptz NOT NULL,
    cpu_avg           real,
    cpu_max           real,
    memory_peak_bytes bigint,
    net_in_bytes      bigint NOT NULL DEFAULT 0,
    net_out_bytes     bigint NOT NULL DEFAULT 0,
    PRIMARY KEY (program, task_id, hour)
);
CREATE INDEX idx_program_hourly_hour ON program_hourly(hour);
