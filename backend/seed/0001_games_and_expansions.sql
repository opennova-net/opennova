-- Seed games + expansions so the Vue lobby/expansions browser has something
-- to render against a fresh DB. Idempotent via INSERT OR IGNORE on UNIQUE slug.
--
-- The data mirrors onnet/alembic/005..013 collapsed.

INSERT OR IGNORE INTO games (
    slug, display_name, lobby_name, gate_tag,
    start_page, host_page, gsb_path,
    join_lan_url, ver1, ver2, exp_bits, pfid,
    executable_name
) VALUES
    ('jop_2_consumer', 'Joint Operations',          'jop_2_consumer', 'jop:cus2',
     'jop_2_start.htm',  'jop_2_host1.htm',  'jop_2.gsb?a=1',
     NULL, '3', '2345', '3', NULL, 'Jointops.exe'),

    ('dfx2_consumer',  'Delta Force Xtreme 2',      'dfx2_consumer',  'dfx2:0:cus:buffy',
     'dfx2_0_start.htm', 'dfx2_0_host1.htm', 'dfx2_0.gsb?a=1',
     'NWJoin.dll?needexpkey=dfx2_0_key2err.htm&success=dfx2_0_join.joi&failure=dfx2_0_main.htm&relay=dfx2_0_relay.htm&msgbase=dfx2_0_msg.htm&nodb=dfx2_0_nodb.htm&pfid=38&mode=Login&gsid=@GSID@&lan=1',
     '1', '9013', '1', '38', 'dfx2.exe');

-- Expansions for jop_2_consumer
INSERT OR IGNORE INTO expansions (
    game_id, slug, display_name, summary, version, package_type, install_subdir, featured
)
SELECT id, 'revx02', 'Tactical Assault Coalition',
       'Coalition expansion package for Joint Operations',
       '2024.05.15', 'zip', 'expansions/revx02', 1
FROM games WHERE slug = 'jop_2_consumer';

INSERT OR IGNORE INTO expansions (
    game_id, slug, display_name, summary, version, package_type, install_subdir, featured
)
SELECT id, 'onjo01', 'OpenNova Demo Mod',
       'OpenNova remaster expansion for Joint Operations.',
       '0.0.0', 'zip', 'expansion/onjo01', 0
FROM games WHERE slug = 'jop_2_consumer';

-- Expansion for dfx2_consumer
INSERT OR IGNORE INTO expansions (
    game_id, slug, display_name, summary, version, package_type, install_subdir, featured
)
SELECT id, 'ondx01', 'OpenNova Demo Mod',
       'OpenNova remaster expansion for Delta Force Xtreme 2.',
       '0.0.0', 'zip', 'expansion/ondx01', 0
FROM games WHERE slug = 'dfx2_consumer';

-- No expansion_files are seeded. Downloadable files (download_url + real sha256 +
-- size) are written only by cutting a release: admin POST /api/admin/expansions/
-- <slug>/release tags the repo, the repo's CI uploads the package to S3 and calls
-- back to /admin/internal/expansions/<slug>/publish. Until then an expansion shows
-- in the catalogue (this seed) but reads as "Not published yet" in the launcher and
-- omits the Download button on the web Expansions page. (A placeholder row with a
-- fake URL / all-zero sha used to live here; it made expansions look installable
-- but 404'd on download, so it was removed.)

-- Default test users so somebody can log in immediately.
-- Mirrors onnet/onnw/seed.py — passwords are bcrypt-hashed at server startup
-- (the standalone server's main() runs the seed.py-equivalent logic). For
-- now we leave the table empty here and let the server insert at boot.
