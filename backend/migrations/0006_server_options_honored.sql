-- The server options nothing in the runtime acts on left the settings schema
-- (engine/formats/gamecfg/server_settings.cpp), and lan_mode left the hosted one: a
-- hosted server runs the NovaWorld channel, which never reads it. A start refuses an
-- option its schema does not offer, so a saved configuration drops them too.
UPDATE hosted_servers
SET configuration = jsonb_set(configuration, '{options}', (configuration->'options') - ARRAY[
        'connection_speed', 'allow_ai', 'access_codes', 'max_team_lives',
        'time_of_day_continuity', 'vote_period', 'choose_spawn', 'third_person_driver',
        'gps_icons', 'wind', 'dropped_weapon_disappear', 'no_drop_weapons',
        'no_respawn_primary', 'difficulty', 'game_type', 'lan_mode'])
WHERE jsonb_typeof(configuration->'options') = 'object';
-- listing_ping is the listing's AllowPing y/n (game.cfg `ping` nonzero): a toggle now.
UPDATE hosted_servers
SET configuration = jsonb_set(configuration, '{options,listing_ping}',
        to_jsonb((configuration->'options'->>'listing_ping')::numeric <> 0))
WHERE jsonb_typeof(configuration->'options'->'listing_ping') = 'number';
