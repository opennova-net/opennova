-- Hosted data sets as rows (ADR 0064; apps/novaworld_common/hosted/data_set_repository.h):
-- one product per data set, a base game or one expansion, uploaded by admins into the
-- deployment's object store as immutable trees; each hosted server pinned to the trees it
-- started on, updated when its owner says. Service policy, not a port.

-- A data set. kind 'base' speaks a title (the gameprofile code opennova-serve's /game takes:
-- 'jo', 'dfx2') and lists in a lobby (game: the games row's slug); kind 'expansion' is one
-- expansion of its parent base (expansion_code: the /exp code and the expansion/<code>/
-- directory, 'jox01'). current_tree_id is the tree new starts take (the newest ready one).
-- The four seeded rows below are what the deployment ran as the fixed catalog; they hold no
-- tree until an admin uploads one.
CREATE TABLE data_sets (
    id                   bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    display_name         text NOT NULL CHECK (display_name <> ''),
    kind                 text NOT NULL CHECK (kind IN ('base', 'expansion')),
    parent_id            bigint REFERENCES data_sets(id) ON DELETE RESTRICT,
    expansion_code       text NOT NULL DEFAULT '',
    title                text NOT NULL DEFAULT '',
    game                 text NOT NULL DEFAULT '',
    current_tree_id      bigint,
    created_by           text NOT NULL DEFAULT '',
    created_at           timestamptz NOT NULL DEFAULT now(),
    updated_at           timestamptz NOT NULL DEFAULT now(),
    CHECK ((kind = 'base') = (parent_id IS NULL AND expansion_code = '' AND title <> '' AND game <> '')),
    CHECK ((kind = 'expansion') = (parent_id IS NOT NULL AND expansion_code <> '' AND title = '' AND game = ''))
);
CREATE UNIQUE INDEX idx_data_sets_expansion ON data_sets(parent_id, expansion_code) WHERE kind = 'expansion';
CREATE INDEX idx_data_sets_parent ON data_sets(parent_id);

-- One version of a data set's files in the store (trees/<id>/...), uploaded by the admin.
-- Its life: uploading (the browser's parts in flight, through web) -> finalizing (the worker
-- checks and measures the files and lists its missions) -> ready; failed with `error`;
-- pruned once no start and no live server needs it (its objects removed when pruned_at is
-- set, by the tree.prune job). manifest holds {"files": [{"name", "size"}]} and missions
-- [{"file", "title"}] once ready (the multiplayer missions its archives list: what the start
-- form offers), version_text the first line of its version.txt.
CREATE TABLE data_set_trees (
    id               bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    data_set_id      bigint NOT NULL REFERENCES data_sets(id) ON DELETE CASCADE,
    state            text NOT NULL
        CHECK (state IN ('uploading', 'finalizing', 'ready', 'failed', 'pruned')),
    version_text     text NOT NULL DEFAULT '',
    manifest         jsonb NOT NULL DEFAULT '{}'::jsonb,
    missions         jsonb NOT NULL DEFAULT '[]'::jsonb,
    bytes            bigint NOT NULL DEFAULT 0,
    error            text NOT NULL DEFAULT '',
    created_by       text NOT NULL DEFAULT '',
    created_at       timestamptz NOT NULL DEFAULT now(),
    ready_at         timestamptz,
    pruned_at        timestamptz
);
CREATE INDEX idx_data_set_trees_data_set ON data_set_trees(data_set_id, id);

ALTER TABLE data_sets
    ADD CONSTRAINT data_sets_current_tree_fkey
        FOREIGN KEY (current_tree_id) REFERENCES data_set_trees(id) ON DELETE SET NULL;

-- A hosted server runs a data set (its base) and, when its owner picked one, an expansion of
-- it; it is pinned to the trees its start took (base_tree_id, expansion_tree_id: NULL before
-- its first start on this schema) and keeps them through crash restarts and moves until
-- its owner asks for an update (update_requested: the supervisor restarts it on the data
-- sets' current trees, then clears the flag). The old game slug and expansion code map onto
-- the seeded rows below, then go.
ALTER TABLE hosted_servers
    ADD COLUMN data_set_id       bigint REFERENCES data_sets(id) ON DELETE SET NULL,
    ADD COLUMN expansion_id      bigint REFERENCES data_sets(id) ON DELETE SET NULL,
    ADD COLUMN base_tree_id      bigint REFERENCES data_set_trees(id) ON DELETE SET NULL,
    ADD COLUMN expansion_tree_id bigint REFERENCES data_set_trees(id) ON DELETE SET NULL,
    ADD COLUMN update_requested  boolean NOT NULL DEFAULT false;

-- Why a container that never started did not (hosted/agent_protocol.h ContainerReport
-- `reason`: its tree could not be fetched), '' for every other report: the supervisor's
-- failure event says so.
ALTER TABLE fleet_slot_reports ADD COLUMN reason text NOT NULL DEFAULT '';

INSERT INTO data_sets (display_name, kind, title, game)
VALUES ('Joint Operations', 'base', 'jo', 'jop_2_consumer'),
       ('Delta Force Xtreme 2', 'base', 'dfx2', 'dfx2_consumer'),
       ('OpenNova', 'base', 'jo', 'opennova_0_consumer');
INSERT INTO data_sets (display_name, kind, parent_id, expansion_code)
SELECT 'Escalation', 'expansion', id, 'jox01'
FROM data_sets WHERE kind = 'base' AND game = 'jop_2_consumer';

UPDATE hosted_servers s SET data_set_id = d.id
FROM data_sets d WHERE d.kind = 'base' AND d.game = s.game;
UPDATE hosted_servers s SET expansion_id = d.id
FROM data_sets d
WHERE d.kind = 'expansion' AND d.parent_id = s.data_set_id AND d.expansion_code = s.expansion
  AND s.expansion <> '';

ALTER TABLE hosted_servers DROP COLUMN game, DROP COLUMN expansion;
CREATE INDEX idx_hosted_servers_data_set ON hosted_servers(data_set_id);
CREATE INDEX idx_hosted_servers_pins ON hosted_servers(base_tree_id, expansion_tree_id)
    WHERE state NOT IN ('stopped', 'failed');
