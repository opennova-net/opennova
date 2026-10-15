-- The games: one row per lobby (apps/novaworld_server/catalog_repository.h).
-- A client's gate tag selects its row; the row decides the gate's answer (the
-- LobbyName, the startupurl, the GLSVSS leg), the .gsb list the browser
-- fetches and the game its listings land under. Applied at every boot after
-- the migrations, so a row added here reaches every database. Idempotent via
-- ON CONFLICT DO NOTHING on the UNIQUE slug and gate_tag: a row already present
-- is left as it is (migration 0013 brings the older rows' values up to these).
--
-- `title` is the engine's game profile code the row's clients speak
-- (engine/base/gameprofile). `startup_query` is the startupurl after
-- "nwprepare.dll?" and the glsvss_* columns the GLSVSS leg, each as the
-- title's own gate answers them: JO's startupurl as a retail JO client
-- fetched it, DFX2's LobbyName, startupurl (pfid=38 in it) and GLSVSS leg as
-- NovaLogic's live gate answered the DFX2 tag on 2026-10-14
-- (docs/titles/dfx2-vs-jo-re.md section 6.5, DFX2-6). The OpenNova row (ON
-- data) speaks JO's wire identity, so its title, version pair, product family
-- (pfid 28), pages and startupurl shape are JO's; its gate tag, LobbyName and
-- .gsb are its own. `exp_bits` is the expansion bits a listing that reports
-- none requires, and that an account with no access row of its own owns:
-- bit 0 the base game, bit 1 JO's jox01; DFX2 and ON have no expansion.

INSERT INTO games (
    slug, display_name, lobby_name, gate_tag, title,
    start_page, host_page, gsb_path,
    join_lan_url, ver1, ver2, exp_bits, pfid,
    startup_query, glsvss_request, glsvss_rims, glsvss_agrms
) VALUES
    ('jop_2_consumer', 'Joint Operations',          'jop_2_consumer', 'jop:cus2', 'jo',
     'jop_2_start.htm',  'jop_2_host1.htm',  'jop_2.gsb?a=1',
     NULL, '3', '2345', '3', '28',
     'ver1=[VER1]&ver2=[VER2]&cc=[CC]&gt=[GT]&url=jop_2_start.htm', '', 0, 0),

    ('dfx2_consumer',  'Delta Force Xtreme 2',      'dfx2_0_consumer', 'dfx2:0:cus:buffy', 'dfx2',
     'dfx2_0_start.htm', 'dfx2_0_host1.htm', 'dfx2_0.gsb?a=1',
     'NWJoin.dll?needexpkey=dfx2_0_key2err.htm&success=dfx2_0_join.joi&failure=dfx2_0_main.htm&relay=dfx2_0_relay.htm&msgbase=dfx2_0_msg.htm&nodb=dfx2_0_nodb.htm&pfid=38&mode=Login&gsid=@GSID@&lan=1',
     '1', '9013', '1', '38',
     'ver1=[VER1]&ver2=[VER2]&cc=[CC]&pfid=38&gt=[GT]&url=dfx2_0_start.htm', '[DFX2]KILLS', 3600000, 45000),

    ('opennova_0_consumer', 'OpenNova',             'opennova_0_consumer', 'opennova:0', 'jo',
     'jop_2_start.htm',  'jop_2_host1.htm',  'opennova_0.gsb?a=1',
     NULL, '3', '2345', '1', '28',
     'ver1=[VER1]&ver2=[VER2]&cc=[CC]&gt=[GT]&url=jop_2_start.htm', '', 0, 0)
ON CONFLICT DO NOTHING;
