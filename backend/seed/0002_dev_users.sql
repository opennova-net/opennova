-- Dev-only seed for a handful of playable accounts. After Phase H, retail's
-- POST to /NWLogin.dll EPASK-decrypts NAME/PASSWORD and looks them up against
-- this table. Round-robin assignment is the curl-walkthrough fallback
-- (no EPASK form field, no auth attempted).
--
-- password_hash is bcrypt $2b$10$ — generated via:
--   python -c "import bcrypt; print(bcrypt.hashpw(b'test', bcrypt.gensalt(rounds=10)).decode())"
-- For dev, the cleartext passwords are intentionally simple: 'test' for the
-- test user, 'foo' for the foo user. test1/test2/test3 reuse the 'test' hash
-- (so their password is also 'test') — they exist so two+ retail clients can
-- log in concurrently on one box to exercise host+browse (one process hosts,
-- the others browse). To rotate, regenerate via the one-liner above.
--
-- PCIDs are the 8-hex-char Player Connection IDs that retail uses to
-- disambiguate accounts; nwhandle is the in-game display name.
INSERT OR IGNORE INTO players (username, password_hash, pcid, nwh, nwhandle)
VALUES
    ('test',
     '$2b$10$biA28xC5Ka.QKKUOKWJGSuCFOfX8D51XkHIZUurZzstBCNQRfXCMS',
     '00000002', '1', 'TestPlayer'),
    ('foo',
     '$2b$10$M0sIsGDx7nXPWw9toIMIa.2RxXZm39nROt2kjsoyi9QUN9Q7x2SIC',
     '00000003', '1', 'FooPlayer'),
    ('test1',
     '$2b$10$biA28xC5Ka.QKKUOKWJGSuCFOfX8D51XkHIZUurZzstBCNQRfXCMS',
     '00000004', '1', 'TestPlayer1'),
    ('test2',
     '$2b$10$biA28xC5Ka.QKKUOKWJGSuCFOfX8D51XkHIZUurZzstBCNQRfXCMS',
     '00000005', '1', 'TestPlayer2'),
    ('test3',
     '$2b$10$biA28xC5Ka.QKKUOKWJGSuCFOfX8D51XkHIZUurZzstBCNQRfXCMS',
     '00000006', '1', 'TestPlayer3');

-- Repair existing development DBs too. Earlier seed versions used
-- INSERT OR IGNORE only, so a stale row (for example foo with an empty
-- nwhandle) would survive every server boot and yield blank in-game names.
UPDATE players SET
    password_hash = '$2b$10$biA28xC5Ka.QKKUOKWJGSuCFOfX8D51XkHIZUurZzstBCNQRfXCMS',
    pcid = '00000002',
    nwh = '1',
    nwhandle = 'TestPlayer',
    account_status = 'active'
WHERE username = 'test';

UPDATE players SET
    password_hash = '$2b$10$M0sIsGDx7nXPWw9toIMIa.2RxXZm39nROt2kjsoyi9QUN9Q7x2SIC',
    pcid = '00000003',
    nwh = '1',
    nwhandle = 'FooPlayer',
    account_status = 'active'
WHERE username = 'foo';

UPDATE players SET
    password_hash = '$2b$10$biA28xC5Ka.QKKUOKWJGSuCFOfX8D51XkHIZUurZzstBCNQRfXCMS',
    pcid = '00000004',
    nwh = '1',
    nwhandle = 'TestPlayer1',
    account_status = 'active'
WHERE username = 'test1';

UPDATE players SET
    password_hash = '$2b$10$biA28xC5Ka.QKKUOKWJGSuCFOfX8D51XkHIZUurZzstBCNQRfXCMS',
    pcid = '00000005',
    nwh = '1',
    nwhandle = 'TestPlayer2',
    account_status = 'active'
WHERE username = 'test2';

UPDATE players SET
    password_hash = '$2b$10$biA28xC5Ka.QKKUOKWJGSuCFOfX8D51XkHIZUurZzstBCNQRfXCMS',
    pcid = '00000006',
    nwh = '1',
    nwhandle = 'TestPlayer3',
    account_status = 'active'
WHERE username = 'test3';

INSERT OR IGNORE INTO player_game_access (user_id, game_slug, status, exp_bits)
SELECT p.id, g.slug, 'active', g.exp_bits
FROM players p
JOIN games g ON g.slug = 'jop_2_consumer'
WHERE p.username IN ('test', 'foo', 'test1', 'test2', 'test3');

UPDATE player_game_access
SET status = 'active',
    exp_bits = (SELECT exp_bits FROM games WHERE slug = 'jop_2_consumer'),
    updated_at = CURRENT_TIMESTAMP
WHERE game_slug = 'jop_2_consumer'
  AND user_id IN (
      SELECT id FROM players
      WHERE username IN ('test', 'foo', 'test1', 'test2', 'test3')
  );
