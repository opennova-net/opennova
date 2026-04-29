-- Dev-only seed for two playable accounts. After Phase H, retail's POST
-- to /NWLogin.dll EPASK-decrypts NAME/PASSWORD and looks them up against
-- this table. Round-robin assignment is the curl-walkthrough fallback
-- (no EPASK form field, no auth attempted).
--
-- password_hash is bcrypt $2b$10$ — generated via:
--   python -c "import bcrypt; print(bcrypt.hashpw(b'test', bcrypt.gensalt(rounds=10)).decode())"
-- For dev, the cleartext passwords are intentionally simple: 'test' for
-- the test user, 'foo' for the foo user. To rotate, regenerate via the
-- one-liner above and replace the values below.
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
     '00000003', '1', 'FooPlayer');

INSERT OR IGNORE INTO player_game_access (user_id, game_slug, status, exp_bits)
SELECT p.id, g.slug, 'active', g.exp_bits
FROM players p
JOIN games g ON g.slug = 'jop_2_consumer'
WHERE p.username IN ('test', 'foo');
