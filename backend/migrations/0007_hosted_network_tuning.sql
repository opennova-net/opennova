-- Launch-time tuning stays in the canonical configuration document. Preserve the packet
-- ceiling formerly stored in options, including the clamp game.cfg applied when it loaded.
-- Existing servers otherwise keep the defaults used before the launcher exposed these fields.
UPDATE hosted_servers
SET configuration = ((jsonb_build_object(
        'send_holdoff_ticks', 12, 'entity_send_budget', 600, 'lock_framerate', true)
        || configuration) #- '{options,max_packet_size}')
    || jsonb_build_object('max_packet_size', COALESCE(configuration->'max_packet_size',
        CASE WHEN jsonb_typeof(configuration->'options'->'max_packet_size') = 'number'
            THEN to_jsonb(GREATEST(100, LEAST(16384,
                (configuration->'options'->>'max_packet_size')::numeric))) END,
        '1300'::jsonb));
