-- OP: Acid Gambit's lobby is distinct from JO's the way DFX2's is: its own product family (pfid
-- 100) and pages (acidgambit_0_*). Its title and version pair stay JO's, the protocol it speaks.
-- The lobbies copied from its row share its pages and pfid: each player's game's (on_<id>) and
-- each one 0003 backfilled (<key>_0_consumer). Every row keeps its tag, LobbyName and .gsb.

UPDATE games SET
    start_page = 'acidgambit_0_start.htm',
    host_page = 'acidgambit_0_host1.htm',
    pfid = '100',
    startup_query = 'ver1=[VER1]&ver2=[VER2]&cc=[CC]&gt=[GT]&url=acidgambit_0_start.htm'
WHERE slug = 'acidgambit_0_consumer'
   OR (title = 'jo' AND slug <> 'jop_2_consumer' AND start_page = 'jop_2_start.htm');
