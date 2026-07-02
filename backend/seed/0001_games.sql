-- Seed games so the Vue lobby/expansions browser and the expansion catalogue
-- have a base to render against a fresh DB. Idempotent via INSERT OR IGNORE on
-- the UNIQUE slug.
--
-- Expansions are NOT seeded here — they come from the Terraform-managed
-- catalogue (infra/github/local.expansions -> backend/seed/0002_expansions.generated.sql),
-- which is the single source of truth for expansion metadata and the slug->repo
-- mapping. This file applies first (lexical order) so that generated seed's
-- `FROM games WHERE slug = ...` subselect resolves.

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
