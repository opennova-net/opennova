-- The update releases (ADR 0064 d5, d9; apps/novaworld_common/hosted/data_set_repository.h):
-- 1SFS packages an admin uploads, each section applied to the baseline of the data set
-- whose command file it carries, and served to retail UPDATE.EXE by the web program's
-- update listener. Over 0011_data_sets.sql's rows. Service policy, not a port.

-- A data set's updater product: updater_login and command_file (JOTRus, JOPTR.CMD; '' for a
-- data set the update service does not serve), retail_updater whether SERVER.LST names
-- ftp.novaworld.com (a retail install through the Switcher) or the deployment's update host.
-- baseline_tree_id is the newest ready baseline (what a release applies to; current_tree_id
-- stays the tree new starts take: a baseline upload, or a published release applied),
-- published_release_id the release the update listener serves.
ALTER TABLE data_sets
    ADD COLUMN updater_login        text NOT NULL DEFAULT '',
    ADD COLUMN command_file         text NOT NULL DEFAULT '',
    ADD COLUMN retail_updater       boolean NOT NULL DEFAULT false,
    ADD COLUMN baseline_tree_id     bigint,
    ADD COLUMN published_release_id bigint,
    ADD CONSTRAINT data_sets_updater_check CHECK ((updater_login = '') = (command_file = ''));
CREATE UNIQUE INDEX idx_data_sets_login ON data_sets(upper(updater_login)) WHERE updater_login <> '';

-- A tree is a baseline the admin uploaded (source 'baseline'), or a published release
-- applied to the baseline it was copied from (source 'release', release_id,
-- baseline_tree_id): building (the worker applies a release) -> ready, or failed with
-- `error`. Every tree so far is a baseline.
ALTER TABLE data_set_trees
    DROP CONSTRAINT data_set_trees_state_check,
    ADD CONSTRAINT data_set_trees_state_check
        CHECK (state IN ('uploading', 'finalizing', 'building', 'ready', 'failed', 'pruned')),
    ADD COLUMN source           text NOT NULL DEFAULT 'baseline' CHECK (source IN ('baseline', 'release')),
    ADD COLUMN release_id       bigint,
    ADD COLUMN baseline_tree_id bigint REFERENCES data_set_trees(id) ON DELETE SET NULL;
ALTER TABLE data_set_trees ALTER COLUMN source DROP DEFAULT;

-- An uploaded 1SFS package (updates/<sha256>.sfs once validated; updates/upload-<id>.sfs
-- while uploading): uploading -> validating (the worker parses it and maps each section to a
-- data set by its command file) -> draft | invalid -> published (its sections' data sets
-- serve it to the updater and get a tree built from it) -> retired. package_product is the
-- 1SFS trailer's; name the admin's label.
CREATE TABLE releases (
    id              bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    status          text NOT NULL DEFAULT 'uploading'
        CHECK (status IN ('uploading', 'validating', 'invalid', 'draft', 'published', 'retired')),
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
    ADD CONSTRAINT data_sets_baseline_tree_fkey
        FOREIGN KEY (baseline_tree_id) REFERENCES data_set_trees(id) ON DELETE SET NULL,
    ADD CONSTRAINT data_sets_published_release_fkey
        FOREIGN KEY (published_release_id) REFERENCES releases(id) ON DELETE SET NULL;
ALTER TABLE data_set_trees
    ADD CONSTRAINT data_set_trees_release_fkey
        FOREIGN KEY (release_id) REFERENCES releases(id) ON DELETE SET NULL;

-- A data set's current tree so far is a baseline upload: it is the baseline a release
-- applies to.
UPDATE data_sets SET baseline_tree_id = current_tree_id WHERE current_tree_id IS NOT NULL;

-- The seeded retail products the update service serves (the Switcher sends their updater to
-- it): Joint Operations and its Escalation expansion.
UPDATE data_sets SET updater_login = 'JOTRus', command_file = 'JOPTR.CMD', retail_updater = true
WHERE kind = 'base' AND game = 'jop_2_consumer';
UPDATE data_sets d SET updater_login = 'JOEus', command_file = 'JOEPTR.CMD', retail_updater = true
FROM data_sets b
WHERE d.kind = 'expansion' AND d.expansion_code = 'jox01' AND d.parent_id = b.id
  AND b.kind = 'base' AND b.game = 'jop_2_consumer';
