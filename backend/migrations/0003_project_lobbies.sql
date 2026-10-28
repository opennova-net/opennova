-- Lobby configuration is service data, established by migrations. Existing tags stay stable.
-- A project selects its lobby before building; clients receive the generated gt.ssc bytes.

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

    ('acidgambit_0_consumer', 'OP: Acid Gambit',      'acidgambit_0_consumer', 'acidgambit:0', 'jo',
     'jop_2_start.htm',  'jop_2_host1.htm',  'acidgambit_0.gsb?a=1',
     NULL, '3', '2345', '1', '28',
     'ver1=[VER1]&ver2=[VER2]&cc=[CC]&gt=[GT]&url=jop_2_start.htm', '', 0, 0)
ON CONFLICT DO NOTHING;

-- Preserve identities already distributed by the old build-before-publish flow. A new
-- project uses a games.id identity; an existing project keeps its configured/predicted one.
INSERT INTO games (slug, display_name, lobby_name, gate_tag, title, start_page, host_page,
                   gsb_path, join_lan_url, ver1, ver2, exp_bits, pfid, startup_query,
                   glsvss_request, glsvss_rims, glsvss_agrms)
SELECT lower(p.key) || '_0_consumer', p.display_name, lower(p.key) || '_0_consumer',
       lower(p.key) || ':0', p.title, t.start_page, t.host_page,
       lower(p.key) || '_0.gsb?a=1', t.join_lan_url, t.ver1, t.ver2, t.exp_bits, t.pfid,
       t.startup_query, t.glsvss_request, t.glsvss_rims, t.glsvss_agrms
FROM products p JOIN games t ON t.slug = CASE p.title
    WHEN 'jo' THEN 'acidgambit_0_consumer' WHEN 'dfx2' THEN 'dfx2_consumer' END
WHERE p.kind = 'game' AND p.owner_id IS NOT NULL AND p.lobby = ''
ON CONFLICT DO NOTHING;
UPDATE products p SET lobby = g.slug
FROM games g WHERE p.kind = 'game' AND p.owner_id IS NOT NULL AND p.lobby = ''
  AND g.slug = lower(p.key) || '_0_consumer' AND g.gate_tag = lower(p.key) || ':0'
  AND g.title = p.title;

-- Existing games may have distributed releases whose rows have since been deleted.
-- Freeze their selection conservatively; newly created games start unlocked.
ALTER TABLE products ADD COLUMN lobby_locked boolean NOT NULL DEFAULT false;
UPDATE products SET lobby_locked = true WHERE kind = 'game';