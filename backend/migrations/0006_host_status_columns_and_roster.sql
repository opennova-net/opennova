-- The remaining retail Host columns and the host-reported roster.
--
-- A retail host's Host var list carries every browser column
-- (Lobby_UpdateServerInfo @0x4fe8c0): TimeLeft, TimeOfDay, Msg, Mod, Age,
-- PBServer, LevelRange, BBMode, Skins, Tracers (plus the ones 0003 added);
-- pix keeps the PIX column for a host whose list carries one. The GSB row
-- used to ship gsb.h defaults for these; now the host-reported value is
-- stored and projected.
--
-- The PlayerList arrives as VarFNum-indexed entries, one slot per player
-- (Server_PlayerAdd @0x51d441..0x51d4aa: PlayerName, PlayerIpAndPort,
-- PlayerPCID, PlayerTeam, PlayerType). host_roster keeps them per slot so the
-- GSB row tail can carry the names the retail browser counts
-- (NapiGameList_ProcessEncryptedResponse @0x63dafc). Distinct from
-- host_players, which correlates our own /NWJoin.dll joiners by peer address.
--
-- Forward-only, like 0005: applies exactly once on fresh and existing DBs.

ALTER TABLE active_hosts ADD COLUMN time_left   TEXT NOT NULL DEFAULT '';
ALTER TABLE active_hosts ADD COLUMN time_of_day TEXT NOT NULL DEFAULT '';
ALTER TABLE active_hosts ADD COLUMN msg         TEXT NOT NULL DEFAULT '';
ALTER TABLE active_hosts ADD COLUMN mod         TEXT NOT NULL DEFAULT '';
ALTER TABLE active_hosts ADD COLUMN age         TEXT NOT NULL DEFAULT '';
ALTER TABLE active_hosts ADD COLUMN pb_server   TEXT NOT NULL DEFAULT '';
ALTER TABLE active_hosts ADD COLUMN level_range TEXT NOT NULL DEFAULT '';
ALTER TABLE active_hosts ADD COLUMN bb_mode     TEXT NOT NULL DEFAULT '';
ALTER TABLE active_hosts ADD COLUMN skins       TEXT NOT NULL DEFAULT '';
ALTER TABLE active_hosts ADD COLUMN tracers     TEXT NOT NULL DEFAULT '';
ALTER TABLE active_hosts ADD COLUMN pix         TEXT NOT NULL DEFAULT '';

CREATE INDEX IF NOT EXISTS idx_active_hosts_host_key ON active_hosts(host_key);

CREATE TABLE IF NOT EXISTS host_roster (
    host_rid       INTEGER NOT NULL REFERENCES active_hosts(rid) ON DELETE CASCADE,
    slot           INTEGER NOT NULL,            -- the ClientVar VarFNum
    player_name    TEXT NOT NULL DEFAULT '',
    ip_and_port    TEXT NOT NULL DEFAULT '',
    pcid           TEXT NOT NULL DEFAULT '',
    team           TEXT NOT NULL DEFAULT '',
    type           TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (host_rid, slot)
);
