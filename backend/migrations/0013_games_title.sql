-- One lobby per title: each games row is a lobby, and `title` names the
-- engine's game profile its clients speak (engine/base/gameprofile: 'jo',
-- 'dfx2'). A client's gate tag selects its row, which decides the LobbyName
-- and the start page the gate answers, the .gsb list its browser fetches and
-- the game its listings land under (apps/novaworld_server/catalog_repository.h).
--
-- The rows themselves are the seed's (backend/seed/0001_games.sql), which
-- every boot applies after the migrations (INSERT OR IGNORE): a fresh database
-- gets every row with its title from there, and an existing one gets the rows
-- it lacks (the OpenNova row) at the boot that runs this migration. The rows
-- the seed already wrote before this column existed take their title here, and
-- JO's row its product family, pfid 28 (the NWJoin.dll query a retail JO
-- client sends, docs/net/novaworld-net-re.md section 7, Wave 6), which the
-- seed now writes too.
--
-- Forward-only, like 0005..0012.

ALTER TABLE games ADD COLUMN title TEXT NOT NULL DEFAULT '';

UPDATE games SET title = 'jo' WHERE slug = 'jop_2_consumer';
UPDATE games SET title = 'dfx2' WHERE slug = 'dfx2_consumer';
UPDATE games SET pfid = '28' WHERE slug = 'jop_2_consumer' AND pfid IS NULL;
