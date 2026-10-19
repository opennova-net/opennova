-- The admin's live view of NovaWorld (apps/novaworld_web/connection_routes.h):
-- who each lobby session is, the server each client plays on, and the task each
-- lobby instance runs as, written by the lobby that holds them
-- (engine/net/novaworld/lobby_repository.h); and a listing's joiners keyed by
-- their account, so its roster is who plays on it now. Live rows only: each
-- goes with its session, its instance or its listing. Service policy, not a
-- port.

-- user_id is the account the session signed in as: the one whose live retail
-- login its Cookie list's LOGINSESSIONTAG named when the lobby first resolved
-- it (active_user_sessions), kept for the session's life however that login
-- ends, and moved only by a later tag that names another account. No foreign
-- key, as host_user_id has none: the lobby writes it from its memory, and an
-- account's delete leaves an id no player has, which the view shows as no one.
--
-- The play columns are its latest ClientPlayRequest's PlaySetup, the join
-- address the .joi handed the client: play_addr its IpAddress:PortNumber,
-- playing_rid the listing at that address when the request came (active_hosts
-- host_ip and host_port; NULL for none: a LAN server, or one no lobby lists),
-- and playing_since when it began playing there. All three clear on its
-- ClientStopPlaying, and go with the session row at its end.
--
-- login_session_tag goes: the view read it alone, to name the account its
-- login was, and user_id names it now. The tag, a secret, stays in the
-- lobby's memory.
ALTER TABLE lobby_sessions
    ADD COLUMN user_id       bigint,
    ADD COLUMN playing_rid   bigint CHECK (playing_rid BETWEEN 1 AND 4294967295),
    ADD COLUMN play_addr     text NOT NULL DEFAULT '',
    ADD COLUMN playing_since timestamptz,
    DROP COLUMN login_session_tag;

-- The task the instance's lobby runs as (program_usage.h find_usage_task: its
-- ECS task id, else its hostname), the key its program_samples rows carry, so
-- the admin's view links an instance to its usage; '' for none.
ALTER TABLE lobby_instances ADD COLUMN task_id text NOT NULL DEFAULT '';

-- A listing's joiners (/NWJoin.dll's rows, which GET /api/lobbies/<rid> lists
-- as its roster) are keyed by the joiner's account, the one its retail login
-- names and its lobby session signed in as: the lobby takes the row off when
-- that session stops playing there, plays elsewhere or ends
-- (hostdb::remove_player). They were keyed by the HTTP address the join came
-- from with a port of 0, which the lobby's UDP address never matched (a row
-- stayed until its listing went) and which two joiners behind one address
-- shared (the later replaced the earlier). A row with no account (its account
-- deleted) goes, one row per account and listing stays (its latest join), and
-- an account's delete now takes its rows.
DELETE FROM host_players WHERE user_id IS NULL;
DELETE FROM host_players a USING host_players b
    WHERE a.host_rid = b.host_rid AND a.user_id = b.user_id
      AND (a.joined_at, a.peer_ip, a.peer_port) < (b.joined_at, b.peer_ip, b.peer_port);
ALTER TABLE host_players DROP CONSTRAINT host_players_pkey;
ALTER TABLE host_players DROP COLUMN peer_port;
ALTER TABLE host_players DROP CONSTRAINT host_players_user_id_fkey;
ALTER TABLE host_players
    ALTER COLUMN user_id SET NOT NULL,
    ADD PRIMARY KEY (host_rid, user_id),
    ADD CONSTRAINT host_players_user_id_fkey
        FOREIGN KEY (user_id) REFERENCES players(id) ON DELETE CASCADE;
