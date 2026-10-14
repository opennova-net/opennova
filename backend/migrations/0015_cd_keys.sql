-- CD keys (the hosted titles program): the keys NovaWorld issues for a title
-- whose client activates against the service, and the machines each one
-- activated. Only Delta Force Xtreme 2 does: its boot and menus gate on a
-- registry CDKIID that NovaLogic's NovaWorld handed out as the NWACT browser
-- variable, bound to the machine's NWPSSK fingerprint, and its LAN host checks
-- the wire CDKIID that value decodes to (docs/titles/dfx2-vs-jo-re.md section
-- 9, DFX2-13). NovaLogic's DFX2 service is dead (NWNODB01), so these stand in
-- for its activations. Joint Operations checks no key and has none here.
-- apps/novaworld_server/cd_keys.h is the code over these tables.
--
--   cd_keys              one key: its title (games.title, 'dfx2'), its serial
--                        (the plaintext it carries, "DFX2-XXXX-XXXX-XXXX-XXXX")
--                        and its wire key (the A-P CDKIID the serial encodes to
--                        under the title's wire chain, which an activated client
--                        keeps and a LAN host checks); the account it is on
--                        (NULL until assigned), who made it ('admin <name>',
--                        'token', or 'auto' for a key minted at an account's
--                        first activation), when, when it was assigned and when
--                        it was revoked (a revoked key activates no machine).
--                        An account holds at most one live key per title.
--   cd_key_activations   one machine a key activated: the NWPSSK fingerprint
--                        its client posts and the NWACT the service handed it,
--                        the same every time that machine asks again.
--
-- A deleted account's keys and their activations go with it; a serial is 64
-- random bits, so none is ever minted again. Forward-only, like 0005..0014.

CREATE TABLE cd_keys (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    title       TEXT NOT NULL,
    serial      TEXT NOT NULL UNIQUE,
    wire_key    TEXT NOT NULL UNIQUE,
    user_id     INTEGER REFERENCES players(id) ON DELETE CASCADE,
    created_by  TEXT NOT NULL,
    created_at  TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    assigned_at TEXT,
    revoked_at  TEXT
);
CREATE INDEX idx_cd_keys_title ON cd_keys(title);
CREATE INDEX idx_cd_keys_user ON cd_keys(user_id);
CREATE UNIQUE INDEX idx_cd_keys_live_per_account ON cd_keys(user_id, title)
    WHERE user_id IS NOT NULL AND revoked_at IS NULL;

CREATE TABLE cd_key_activations (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    key_id     INTEGER NOT NULL REFERENCES cd_keys(id) ON DELETE CASCADE,
    nwpssk     TEXT NOT NULL,
    nwact      TEXT NOT NULL,
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE (key_id, nwpssk)
);
