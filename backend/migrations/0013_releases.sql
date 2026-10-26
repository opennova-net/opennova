-- The update releases (ADR 0064 d5, d9; apps/novaworld_common/data_sets/data_set_repository.h):
-- 1SFS packages, each section a whole build of the data set whose command file it carries,
-- served to retail UPDATE.EXE by the web program's update listener and built into that data
-- set's tree from nothing (release.apply). A game with a project builds its releases from its
-- revisions (0025); one with none (OpenNova's runtime) has its packages uploaded. Over
-- 0011_data_sets.sql's rows. Service policy, not a port.

-- A data set's updater product: updater_login and command_file (JOTRus, JOPTR.CMD; '' for a
-- data set the update service does not serve); retail whether it is a retail game (NovaLogic's
-- install: its SERVER.LST names ftp.novaworld.com, reached through the Switcher, where our own
-- game's names the deployment's update host; its project's revision 1 the stock install, 0025).
-- published_release_id is the release the update listener serves and its tree is built from
-- (current_tree_id the tree new starts take).
ALTER TABLE data_sets
    ADD COLUMN updater_login        text NOT NULL DEFAULT '',
    ADD COLUMN command_file         text NOT NULL DEFAULT '',
    ADD COLUMN retail               boolean NOT NULL DEFAULT false,
    ADD COLUMN published_release_id bigint,
    ADD CONSTRAINT data_sets_updater_check CHECK ((updater_login = '') = (command_file = '')),
    -- A standalone updater product: a login and its command file, and nothing a game has.
    ADD CONSTRAINT data_sets_update_check CHECK (kind <> 'update' OR (
        parent_id IS NULL AND expansion_code = '' AND title = '' AND game = '' AND
        updater_login <> '' AND current_tree_id IS NULL));
CREATE UNIQUE INDEX idx_data_sets_login ON data_sets(upper(updater_login)) WHERE updater_login <> '';

-- The release a tree was built from (NULL once that release is deleted).
ALTER TABLE data_set_trees ADD COLUMN release_id bigint;

-- A 1SFS package (updates/<sha256>.sfs once validated; updates/upload-<id>.sfs before):
-- uploading -> validating (the worker parses it and maps each section to a data set by its
-- command file) -> draft | invalid -> published (its sections' data sets serve it to the
-- updater and get a tree built from it) -> retired. data_set_id is the game it is a release of
-- (a built one's from the start, an upload's once its validation maps its base section);
-- package_product the 1SFS trailer's; name the release's label.
CREATE TABLE releases (
    id              bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    status          text NOT NULL DEFAULT 'uploading'
        CHECK (status IN ('uploading', 'validating', 'invalid', 'draft', 'published', 'retired')),
    data_set_id     bigint REFERENCES data_sets(id) ON DELETE SET NULL,
    sha256          text NOT NULL DEFAULT '',
    bytes           bigint NOT NULL DEFAULT 0,
    package_product text NOT NULL DEFAULT '',
    name            text NOT NULL DEFAULT '',
    error           text NOT NULL DEFAULT '',
    created_by      text NOT NULL DEFAULT '',
    created_at      timestamptz NOT NULL DEFAULT now(),
    published_at    timestamptz,
    retired_at      timestamptz
);
CREATE UNIQUE INDEX idx_releases_sha ON releases(sha256) WHERE sha256 <> '';

-- One section of a package (the base product's, then one per PACKAGEBREAK), mapped to the
-- data set whose command_file it carries; version the command file's VERSION; tree_id the
-- tree its publish built (NULL until the release.apply job made it).
CREATE TABLE release_sections (
    release_id   bigint NOT NULL REFERENCES releases(id) ON DELETE CASCADE,
    data_set_id  bigint NOT NULL REFERENCES data_sets(id) ON DELETE CASCADE,
    expansion    text NOT NULL DEFAULT '',
    command_file text NOT NULL,
    version      text NOT NULL DEFAULT '',
    row_count    bigint NOT NULL DEFAULT 0,
    blob_bytes   bigint NOT NULL DEFAULT 0,
    tree_id      bigint REFERENCES data_set_trees(id) ON DELETE SET NULL,
    PRIMARY KEY (release_id, data_set_id)
);

ALTER TABLE data_sets
    ADD CONSTRAINT data_sets_published_release_fkey
        FOREIGN KEY (published_release_id) REFERENCES releases(id) ON DELETE SET NULL;
ALTER TABLE data_set_trees
    ADD CONSTRAINT data_set_trees_release_fkey
        FOREIGN KEY (release_id) REFERENCES releases(id) ON DELETE SET NULL;

-- The seeded retail products the update service serves (the Switcher sends their updater to
-- it): Joint Operations and its Escalation expansion.
UPDATE data_sets SET updater_login = 'JOTRus', command_file = 'JOPTR.CMD', retail = true
WHERE kind = 'base' AND game = 'jop_2_consumer';
UPDATE data_sets d SET updater_login = 'JOEus', command_file = 'JOEPTR.CMD', retail = true
FROM data_sets b
WHERE d.kind = 'expansion' AND d.expansion_code = 'jox01' AND d.parent_id = b.id
  AND b.kind = 'base' AND b.game = 'jop_2_consumer';
-- Delta Force Xtreme 2's, as the Steam DFX2's UPDATE.WIZ names it (CMD=DFX2.CMD,
-- PRODUCT_LOGIN=DFX2).
UPDATE data_sets SET updater_login = 'DFX2', command_file = 'DFX2.CMD', retail = true
WHERE kind = 'base' AND game = 'dfx2_consumer';

-- OpenNova's own products (ADR 0067): its runtime, a standalone updater product; its game's
-- (OP: Acid Gambit) and its nifty expansion's (OP: Nifty Package), complete packages served at
-- the deployment's update host.
INSERT INTO data_sets (display_name, kind, updater_login, command_file)
VALUES ('OpenNova runtime', 'update', 'ONRUNTIME', 'ONRUNTIME.CMD');
UPDATE data_sets SET updater_login = 'ACIDGAMBIT', command_file = 'ACIDGAMBIT.CMD'
WHERE kind = 'base' AND game = 'acidgambit_0_consumer';
INSERT INTO data_sets (display_name, kind, parent_id, expansion_code, updater_login, command_file)
SELECT 'OP: Nifty Package', 'expansion', id, 'nifty', 'NIFTY', 'NIFTY.CMD'
FROM data_sets WHERE kind = 'base' AND game = 'acidgambit_0_consumer';
