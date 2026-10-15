-- The NovaWorld service's schema (PostgreSQL 17), squashed from the SQLite
-- migrations 0001..0015 it replaces: their end state, in this dialect. Applied
-- once per database by opennova::db::run_migrations (database.h), which records
-- this file's SHA-256: from the cutover on, migrations are append-only, so a
-- change is a new file (0002_*.sql, ...), never an edit here.
--
-- Dialect rules every later file follows:
--   - integers are bigint (an id, a count, a port, a RID: SQLite's INTEGER was
--     64-bit, and a u32 RID does not fit an integer); a flag is boolean
--   - an id is bigint GENERATED ALWAYS AS IDENTITY: a deleted row's id is never
--     handed out again, so nothing that kept one (a listing's host_user_id in a
--     lobby's memory, a log line) ever names a newcomer
--   - a time is timestamptz, written now() (the transaction's start); the
--     service reads it as "YYYY-MM-DD HH:MM:SS" UTC text
--   - the database is SQL_ASCII with C collation and ctype (run_migrations
--     checks): text is the bytes a client sent (a stock client's cp1252
--     included), compared and ordered byte by byte, lower() ASCII only

-- Players (accounts). kind 'service' is a hosted server's own account
-- (srv-<id>), minted by the supervisor with a random password, set again at
-- every start of the server's process and deleted once the server has stopped
-- or failed; the website's login refuses one, the admin users list hides it,
-- and no person may register a name or handle starting with srv-
-- (apps/novaworld_server/auth.h). account_status is 'active' or the name of
-- whatever else the account is ('banned', ...), never empty. role is the
-- website role. credentials_gen moves (+1) with every admin update that sets a
-- password or a status other than 'active' (auth.cpp update_user), in the
-- statement that makes the change: every login's grants (a website session,
-- the retail login's active_user_sessions row, a known login address) are
-- written only while the account still has the generation its password check
-- read. comp_servers is an admin's grant of hosted servers (0 to 16): the
-- account's hosting entitlement is the larger of it and its support tier's
-- servers (0002_billing.sql; apps/novaworld_server/billing/entitlement.h).
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
    comp_servers    bigint NOT NULL DEFAULT 0 CHECK (comp_servers BETWEEN 0 AND 16)
);
CREATE INDEX idx_players_nwhandle ON players(nwhandle);

-- Games: one row per lobby (apps/novaworld_server/catalog_repository.h). A
-- client's gate tag selects its row, which decides the gate's answer (the
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

-- The listed game servers (the lobby browser, /jop_2.gsb, /api/hosts). A
-- ClientHostRequest upserts its row, a ClientHostUpdate refreshes it, GOODBYE,
-- a stop or the heartbeat timeout deletes it, and the boot wipes the table (a
-- previous run's hosts are gone). The text columns are the host's Host var
-- list as it sent them (a stock host's cp1252 bytes); host_key / pcid_key are
-- its HostKey and PCIDKey. host_user_id is the owner: the account while one of
-- its hosting keys stands claimed for this RID (host_keys), else NULL.
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
    pix          text NOT NULL DEFAULT ''
);
CREATE INDEX idx_active_hosts_game ON active_hosts(game);
CREATE INDEX idx_active_hosts_peer ON active_hosts(peer_ip, peer_port);
CREATE INDEX idx_active_hosts_host_key ON active_hosts(host_key);
CREATE INDEX idx_active_hosts_host_user ON active_hosts(host_user_id);

-- Our own /NWJoin.dll joiners of a listed host, correlated by peer address.
CREATE TABLE host_players (
    host_rid  bigint NOT NULL REFERENCES active_hosts(rid) ON DELETE CASCADE,
    user_id   bigint REFERENCES players(id) ON DELETE SET NULL,
    nwhandle  text NOT NULL,
    peer_ip   text NOT NULL,
    peer_port bigint NOT NULL,
    joined_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (host_rid, peer_ip, peer_port)
);
CREATE INDEX idx_host_players_peer ON host_players(peer_ip, peer_port);
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
-- use (apps/novaworld_server/web_session.cpp); the main tick prunes rows past
-- it. id orders sessions minted at one instant.
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

-- Hosted game servers (ADR 0053): one row per start an owner asks for, walked
-- through requested -> provisioning -> booting -> listing -> running ->
-- stopping -> stopped | failed by the supervisor (apps/novaworld_server/
-- hosted/; a crash restart goes back to provisioning). The owner's settings
-- are columns the backend range-checked (hosted_settings.h); the supervisor
-- renders the server's files from them and never takes a file from the owner.
-- game is the slug of the games row the server lists under (its data set,
-- hosted/data_sets.h). An owner has as many live servers (any state but
-- stopped and failed) as its hosting entitlement allows, and own_machine marks
-- one that runs on a machine of its own (the own-machine add-on); those counts
-- and the global cap (ONNET_HOSTED_CAP) are taken in the transaction that
-- inserts a request (hosted::request_server), which holds the writer lock.
-- admin_user / admin_password are the remote-admin login the rendered
-- admin.cfg grants the service alone (a new one at every start). machine_id
-- is the MachineProvider's handle for the machine (provider names which one).
-- owner_user_id and service_user_id are set NULL when the account goes: the
-- supervisor stops a live server whose owner is gone.
CREATE TABLE hosted_servers (
    id              bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    owner_user_id   bigint REFERENCES players(id) ON DELETE SET NULL,
    service_user_id bigint REFERENCES players(id) ON DELETE SET NULL,
    state           text NOT NULL DEFAULT 'requested'
        CHECK (state IN ('requested', 'provisioning', 'booting', 'listing', 'running',
                         'stopping', 'stopped', 'failed')),
    -- The owner's settings, range-checked (hosted_settings.h).
    game_name       text NOT NULL,
    max_players     bigint NOT NULL,
    game_password   text NOT NULL DEFAULT '',
    time_limit      bigint NOT NULL,
    kill_limit      bigint NOT NULL,
    max_score       bigint NOT NULL,
    respawn         bigint NOT NULL,
    start_delay     bigint NOT NULL,
    team_ff         boolean NOT NULL DEFAULT false,
    expansion       text NOT NULL DEFAULT '',  -- '' (base Joint Operations) or 'jox01'
    missions        text NOT NULL,             -- the rotation, one file name per line
    -- The machine.
    provider        text NOT NULL DEFAULT '',
    machine_id      text NOT NULL DEFAULT '',
    machine_address text NOT NULL DEFAULT '',
    admin_port      bigint NOT NULL DEFAULT 0,
    admin_user      text NOT NULL DEFAULT '',
    admin_password  text NOT NULL DEFAULT '',
    restarts        bigint NOT NULL DEFAULT 0,
    failure_reason  text NOT NULL DEFAULT '',
    created_at      timestamptz NOT NULL DEFAULT now(),
    updated_at      timestamptz NOT NULL DEFAULT now(),
    running_at      timestamptz,
    ended_at        timestamptz,
    game            text NOT NULL DEFAULT 'jop_2_consumer',
    own_machine     boolean NOT NULL DEFAULT false
);
CREATE INDEX idx_hosted_servers_owner ON hosted_servers(owner_user_id);
CREATE INDEX idx_hosted_servers_state ON hosted_servers(state);
CREATE INDEX idx_hosted_servers_service_user ON hosted_servers(service_user_id);

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
-- (hosted/admin_link.h), and its console is this ring, one row per line:
-- a command the link sent for an owner, an admin or the service (actor names
-- which), a reply packet (a PLAYER LIST table is one row), or the link's own
-- status lines. The text is UTF-8 (the server's cp1252 decoded); the admin
-- password never reaches it. Every append keeps a server's newest 500 rows
-- (hosted::append_console).
CREATE TABLE hosted_server_console (
    id        bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    server_id bigint NOT NULL REFERENCES hosted_servers(id) ON DELETE CASCADE,
    kind      text NOT NULL CHECK (kind IN ('command', 'reply', 'status')),
    actor     text NOT NULL DEFAULT '' CHECK (actor IN ('', 'owner', 'admin', 'service')),
    text      text NOT NULL,
    at        timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX idx_hosted_server_console_server ON hosted_server_console(server_id, id);

-- CD keys: the keys NovaWorld issues for a title whose client activates
-- against the service, and the machines each one activated. Only Delta Force
-- Xtreme 2 does: its boot and menus gate on a registry CDKIID that NovaLogic's
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
-- the same every time that machine asks again
-- (apps/novaworld_server/cd_keys.h).
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
