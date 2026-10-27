-- Players' own projects (ADR 0073, ADR 0074) and the Switcher's retirement (ADR 0075), on the
-- schema 0001_baseline.sql made: an account makes, pushes, releases and hosts projects of its
-- own, an owner's release waits for an admin's approval, and the ONSWITCHER app goes. The first
-- migration after the baseline: from it on the schema changes by new files alone, and a
-- recorded file never changes (backend/README.md "Migrations"; 0001's header predates the
-- rule). Applies on a database that holds rows. Service policy, not a port.

-- A product's owner (owner_id): the account whose own project it is, a game or an expansion (a
-- mod) any signed-in account may make and push, never an app and never a retail game; NULL for
-- the service's own (every seeded row, and every one an admin makes). It goes with its account
-- (ON DELETE CASCADE: the account's delete queues each one's product.purge first, as a
-- product's own delete does). Its key is at most 11 characters with no QQ, and its updater
-- product is its key (updater_login KEY, command_file KEY.CMD: 1SFS's 15 characters, and no
-- territory placeholder for the updater to replace), its download, a public game's,
-- player-<key>-game-windows.zip, each given at its create
-- (apps/novaworld_web/products/project_routes.h). Its owner releases it; an own game's first
-- publish gives it its own lobby (a games row, product_repository.h publish_release), so a game
-- lists in none until then ('' for none: products_check no longer asks for one).
ALTER TABLE products
    ADD COLUMN owner_id bigint REFERENCES players(id) ON DELETE CASCADE,
    DROP CONSTRAINT products_check,
    ADD CONSTRAINT products_check
        CHECK ((kind = 'game') = (parent_id IS NULL AND expansion_code = '' AND title <> '')),
    -- An account's own is a game or an expansion, never a retail one.
    ADD CONSTRAINT products_owner_check CHECK (owner_id IS NULL OR (kind <> 'app' AND NOT retail));
CREATE INDEX idx_products_owner ON products(owner_id) WHERE owner_id IS NOT NULL;

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
