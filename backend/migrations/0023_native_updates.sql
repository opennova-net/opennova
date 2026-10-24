-- A standalone updater product reuses the release/catalog rows without pretending to be
-- a hosted game: no lobby, title, expansion parent or data trees. Existing base/expansion
-- products keep their shape and updater identities.
ALTER TABLE data_sets
    DROP CONSTRAINT data_sets_kind_check,
    DROP CONSTRAINT data_sets_check,
    DROP CONSTRAINT data_sets_check1,
    ADD CONSTRAINT data_sets_kind_check CHECK (kind IN ('base', 'expansion', 'update')),
    ADD CONSTRAINT data_sets_shape_check CHECK (
        (kind = 'base' AND parent_id IS NULL AND expansion_code = '' AND title <> '' AND game <> '') OR
        (kind = 'expansion' AND parent_id IS NOT NULL AND expansion_code <> '' AND title = '' AND game = '') OR
        (kind = 'update' AND parent_id IS NULL AND expansion_code = '' AND title = '' AND game = ''
            AND current_tree_id IS NULL AND baseline_tree_id IS NULL)
    );

INSERT INTO data_sets (display_name, kind, updater_login, command_file)
VALUES ('OpenNova runtime', 'update', 'ONRUNTIME', 'ONRUNTIME.CMD');

UPDATE data_sets SET updater_login = 'OPENNOVA', command_file = 'OPENNOVA.CMD', retail_updater = false
WHERE kind = 'base' AND game = 'opennova_0_consumer' AND updater_login = '';

UPDATE data_sets SET updater_login = 'DFX2', command_file = 'DFX2.CMD', retail_updater = true
WHERE kind = 'base' AND game = 'dfx2_consumer' AND updater_login = '';

INSERT INTO data_sets (display_name, kind, parent_id, expansion_code, updater_login, command_file)
SELECT 'OpenNova Joint Operations', 'expansion', id, 'onjo1', 'ONJO1', 'ONJO1.CMD'
FROM data_sets WHERE kind = 'base' AND game = 'opennova_0_consumer'
ON CONFLICT (parent_id, expansion_code) WHERE kind = 'expansion' DO NOTHING;

UPDATE data_sets d SET updater_login = 'ONJO1', command_file = 'ONJO1.CMD', retail_updater = false
FROM data_sets b
WHERE d.kind = 'expansion' AND d.expansion_code = 'onjo1' AND d.parent_id = b.id
  AND b.kind = 'base' AND b.game = 'opennova_0_consumer' AND d.updater_login = '';
