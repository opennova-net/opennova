-- Players' own projects released and hosted (ADR 0074), each session's executable (engine PR
-- #1277) and the Switcher's retirement (ADR 0075), on the schema 0001_baseline.sql made (as
-- 58bf5f91f8 left it, the bytes staging recorded at its reset: ADR 0073's owners, owner_id and
-- its check and index, already in it): an account releases and hosts projects of its own, an
-- owner's release waits for an admin's approval, and the ONSWITCHER app goes. The first
-- migration after the baseline: from it on the schema changes by new files alone, and a
-- recorded file never changes (backend/README.md "Migrations"; 0001's header predates the
-- rule). Applies on a database that holds rows. Service policy, not a port.

-- An account's own project's updater product is its key (updater_login KEY, command_file
-- KEY.CMD), and a game's download player-<key>-game-windows.zip, each given at its create now
-- (apps/novaworld_web/products/project_routes.h): one made before this file gets them here,
-- where its key takes them (at most 11 characters with no QQ, so KEY.CMD fits 1SFS's 15 and no
-- wizard value holds the territory placeholder; no other product's login or command file the
-- same, without case). One that cannot stays without, its release refused until an admin gives
-- it an updater product (PATCH /api/admin/products/<id>).
UPDATE products p SET updater_login = p.key, command_file = p.key || '.CMD'
WHERE p.owner_id IS NOT NULL AND p.updater_login = '' AND length(p.key) <= 11
  AND upper(p.key) NOT LIKE '%QQ%'
  AND NOT EXISTS (SELECT 1 FROM products o WHERE o.id <> p.id
                  AND (upper(o.updater_login) = upper(p.key)
                       OR upper(o.command_file) = upper(p.key || '.CMD')));
UPDATE products SET download = 'player-' || lower(key) || '-game-windows.zip'
WHERE owner_id IS NOT NULL AND kind = 'game' AND download = '';

-- An expansion's code names its expansion/<code>/ folder in any game's install: unique across
-- every product without case (it was unique under its parent alone), so no two mods of any
-- games share a folder name. A command file is unique without case too, so no product's can
-- break another's validation (product_jobs.h release.validate maps a section by it).
DROP INDEX idx_products_expansion;
CREATE UNIQUE INDEX idx_products_expansion ON products(lower(expansion_code)) WHERE expansion_code <> '';
CREATE UNIQUE INDEX idx_products_command_file ON products(upper(command_file)) WHERE command_file <> '';

-- A release of an account's own project that its owner (no admin) publishes waits for an
-- admin's approval: draft -> awaiting_approval, which nothing serves, applies or lists; an
-- admin's publish makes it published, an admin's reject a draft again with review_note saying
-- why ('' for none), and its owner may delete it meanwhile. A project's releases are read by
-- its product (idx_releases_product: its owner's release list and its usage).
ALTER TABLE releases
    DROP CONSTRAINT releases_status_check,
    ADD CONSTRAINT releases_status_check CHECK (status IN
        ('uploading', 'building', 'validating', 'invalid', 'draft', 'published', 'retired',
         'awaiting_approval')),
    ADD COLUMN review_note text NOT NULL DEFAULT '';
CREATE INDEX idx_releases_product ON releases(product_id, id);

-- How many releases an account started on its UTC day (day: that day's midnight): the owner
-- release route's daily cap (apps/novaworld_web/products/project_routes.h kReleasesPerDay),
-- counted apart from the releases so a deleted one still counts.
CREATE TABLE release_starts (
    player_id bigint PRIMARY KEY REFERENCES players(id) ON DELETE CASCADE,
    day       timestamptz NOT NULL,
    started   bigint NOT NULL CHECK (started > 0)
);

-- A lobby session's ap: the admitting Hello's AP, the client's executable as its menu reads it
-- ('JOINTOPS.EXE' or 'OPENNOVA.EXE': the client's own text, shown, never trusted; engine PR
-- #1277, which wrote it into 0001 before the rule above, moved here).
ALTER TABLE lobby_sessions ADD COLUMN ap text NOT NULL DEFAULT '';

-- The Switcher retires (ADR 0075): its product goes as a product's delete takes one
-- (product_jobs.h delete_product_purged). Its releases go with it (their sections cascade, its
-- published_release_id clears; it has no tree, no project and no server), their objects (its
-- build as uploaded and its download, each upload key) removed by the data program's
-- product.purge, queued here as the delete queues it, kProductPurgeDelaySeconds on, and the
-- downloads host rendered again (downloads.publish), which drops its zip and its manifest
-- entry. Neither job is queued where it had no release (a database made from these files).
INSERT INTO jobs (kind, payload, dedupe_key, max_attempts, run_at)
SELECT 'product.purge',
       jsonb_build_object('product_id', p.id, 'trees', '[]'::jsonb, 'objects',
           (SELECT COALESCE(jsonb_agg(o.key ORDER BY o.key), '[]'::jsonb)
            FROM releases r, LATERAL (VALUES
                ('updates/upload-' || r.id || '.sfs'),
                ('updates/source-' || r.id || '.zip'),
                (CASE WHEN r.sha256 <> '' THEN 'updates/' || r.sha256 || '.sfs' END),
                (CASE WHEN r.sha256 <> '' THEN 'updates/' || r.sha256 || '.zip' END)) AS o(key)
            WHERE r.product_id = p.id AND o.key IS NOT NULL)),
       'product.purge:' || p.id, 10, now() + interval '75 minutes'
FROM products p
WHERE p.key = 'ONSWITCHER' AND EXISTS (SELECT 1 FROM releases r WHERE r.product_id = p.id)
ON CONFLICT DO NOTHING;
INSERT INTO jobs (kind, dedupe_key, max_attempts)
SELECT 'downloads.publish', 'downloads.publish', 10
WHERE EXISTS (SELECT 1 FROM releases r JOIN products p ON p.id = r.product_id
              WHERE p.key = 'ONSWITCHER')
ON CONFLICT DO NOTHING;
DELETE FROM releases WHERE product_id = (SELECT id FROM products WHERE key = 'ONSWITCHER');
DELETE FROM products WHERE key = 'ONSWITCHER';
