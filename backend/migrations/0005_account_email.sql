-- Account email (apps/novaworld_server/README.md "Account email and password
-- resets"): the address a player registers with, which the welcome mail and the
-- password reset links go to. Service policy, not a port.

-- players.email: the address as normalize_email (auth.h) stores it, its ASCII
-- letters lowercased (lower() folds ASCII alone under the C ctype, the same
-- fold), so one address names one account however it was typed. NULL for an
-- account with none: one an admin made without one, a hosted server's service
-- account, every account made before this column. Nothing is backfilled.
ALTER TABLE players ADD COLUMN IF NOT EXISTS email text
    CHECK (email IS NULL OR (email <> '' AND email = lower(email)));
CREATE UNIQUE INDEX IF NOT EXISTS idx_players_email ON players(email);

-- A password reset an account asked for (POST /api/auth/forgot): the SHA-256 of
-- its link's token (32 random bytes, as 64 hex digits; the link alone carries the
-- token), live until expires_at (an hour). One per account: a newer request
-- replaces it, and POST /api/auth/reset deletes it as it sets the password, so a
-- link works once. Pruned by the main tick's sweep once expired.
CREATE TABLE password_resets (
    user_id    bigint PRIMARY KEY REFERENCES players(id) ON DELETE CASCADE,
    token_hash text NOT NULL UNIQUE,
    created_at timestamptz NOT NULL DEFAULT now(),
    expires_at timestamptz NOT NULL
);
CREATE INDEX idx_password_resets_expires ON password_resets(expires_at);
