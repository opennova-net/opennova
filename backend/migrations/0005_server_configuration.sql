-- Every gameplay setting travels together, including rotations and advanced
-- game.cfg options. Product identities remain foreign keys on the server row.
ALTER TABLE hosted_servers ADD COLUMN configuration jsonb NOT NULL DEFAULT '{}';
ALTER TABLE hosted_servers ADD COLUMN settings_revision bigint NOT NULL DEFAULT 1;
UPDATE hosted_servers SET configuration = jsonb_build_object(
    'game_name', game_name, 'max_players', max_players, 'game_password', game_password,
    'time_limit', time_limit, 'kill_limit', kill_limit, 'max_score', max_score,
    'respawn', respawn, 'start_delay', start_delay, 'team_ff', team_ff,
    'options', jsonb_build_object('replay', true),
    'rotation', COALESCE((SELECT jsonb_agg(jsonb_build_object('file', m, 'swap_sides', false) ORDER BY n)
        FROM unnest(string_to_array(missions, E'\n')) WITH ORDINALITY AS x(m, n)
        WHERE m <> ''), '[]'::jsonb));
ALTER TABLE hosted_servers
    DROP COLUMN game_name, DROP COLUMN max_players, DROP COLUMN game_password,
    DROP COLUMN time_limit, DROP COLUMN kill_limit, DROP COLUMN max_score,
    DROP COLUMN respawn, DROP COLUMN start_delay, DROP COLUMN team_ff, DROP COLUMN missions;
ALTER TABLE hosted_servers ADD CONSTRAINT hosted_configuration_object CHECK (jsonb_typeof(configuration) = 'object');
ALTER TABLE product_trees ADD COLUMN weapons jsonb NOT NULL DEFAULT '[]';
-- Durable completion, pinned to the game process and configuration revision.
ALTER TABLE hosted_console_commands ADD COLUMN server_generation bigint;
ALTER TABLE hosted_console_commands ADD COLUMN settings_revision bigint;
ALTER TABLE hosted_console_commands ADD COLUMN settings_patch jsonb;
ALTER TABLE hosted_console_commands ADD COLUMN configuration_saved boolean NOT NULL DEFAULT false;
ALTER TABLE hosted_console_commands ADD COLUMN delivery_unknown boolean NOT NULL DEFAULT false;
CREATE INDEX hosted_console_server_order ON hosted_console_commands(server_id, id) WHERE state IN ('queued', 'running');
-- Populate catalogs for already published and pinned trees without rebuilding releases.
INSERT INTO jobs(kind, payload, dedupe_key)
SELECT 'tree.server_catalog', jsonb_build_object('tree_id', id), 'tree.server_catalog:' || id
FROM product_trees WHERE state = 'ready';
