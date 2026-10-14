-- One lobby per title: each games row is a lobby, and a client's gate tag
-- selects its row, which decides the gate's answer (the LobbyName, the
-- startupurl, the GLSVSS leg), the .gsb list its browser fetches and the game
-- its listings land under (apps/novaworld_server/catalog_repository.h).
--
--   title          the engine's game profile code the row's clients speak
--                  (engine/base/gameprofile: 'jo', 'dfx2')
--   startup_query  the gate's startupurl after "nwprepare.dll?", as the
--                  title's own gate answers it: JO's names no pfid, DFX2's
--                  carries pfid=38 between cc and gt
--   glsvss_*       the gate's GLSVSSREQUEST / GLSVSSRIMS / GLSVSSAGRMS; an
--                  empty request sends none (the service's configured ones,
--                  ONNET_GLSVSS_*, stand in for a row with none)
--
-- The rows themselves are the seed's (backend/seed/0001_games.sql), which
-- every boot applies after the migrations (INSERT OR IGNORE): a fresh database
-- gets every row from there, and an existing one gets the rows it lacks (the
-- OpenNova row) at the boot that runs this migration. The rows the seed wrote
-- before these columns existed take their values here:
--
--   - JO's pfid 28, the NWJoin.dll query a retail JO client sends
--     (docs/net/novaworld-net-re.md section 7, Wave 6), and its startupurl
--     query, the one this gate always answered (a retail JO client fetched
--     it as nwprepare.dll?ver1=3&ver2=2345&cc=us&gt=jop:cus2&url=jop_2_start.htm);
--   - DFX2's LobbyName, startupurl query and GLSVSS leg as NovaLogic's live
--     gate answered the DFX2 tag on 2026-10-14 (docs/titles/dfx2-vs-jo-re.md
--     section 6.5, DFX2-6): LobbyName dfx2_0_consumer, where the seed had
--     dfx2_consumer. Its slug stays dfx2_consumer.
--
-- Forward-only, like 0005..0012.

ALTER TABLE games ADD COLUMN title TEXT NOT NULL DEFAULT '';
ALTER TABLE games ADD COLUMN startup_query TEXT NOT NULL DEFAULT '';
ALTER TABLE games ADD COLUMN glsvss_request TEXT NOT NULL DEFAULT '';
ALTER TABLE games ADD COLUMN glsvss_rims INTEGER NOT NULL DEFAULT 0;
ALTER TABLE games ADD COLUMN glsvss_agrms INTEGER NOT NULL DEFAULT 0;

UPDATE games SET
    title = 'jo',
    pfid = COALESCE(pfid, '28'),
    startup_query = 'ver1=[VER1]&ver2=[VER2]&cc=[CC]&gt=[GT]&url=jop_2_start.htm'
WHERE slug = 'jop_2_consumer';

UPDATE games SET
    title = 'dfx2',
    lobby_name = 'dfx2_0_consumer',
    startup_query = 'ver1=[VER1]&ver2=[VER2]&cc=[CC]&pfid=38&gt=[GT]&url=dfx2_0_start.htm',
    glsvss_request = '[DFX2]KILLS',
    glsvss_rims = 3600000,
    glsvss_agrms = 45000
WHERE slug = 'dfx2_consumer';
