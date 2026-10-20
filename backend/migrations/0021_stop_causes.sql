-- Who stopped a hosted server, and why the owners' limits did
-- (apps/novaworld_common/hosted/model/hosted_repository.h StartCause, StopLimit), so its
-- owner reads it. Service policy, not a port.

-- A start ends 'owner_stop' for its owner's stop and 'admin_stop' for an admin's, the
-- cause set when the stop is asked. 'stop' stays the service's own (its owner's account
-- deleted or no longer active) and every stop before this file. A limits stop also
-- keeps the limit its server was past: its owner's servers on shared machines
-- ('shared_servers'), or its dedicated machines with no shared server free to move it to
-- ('no_shared_server') or no room on the shared machines ('no_shared_room'), and how many
-- its owner had then (limit_count). One before this file has neither.
ALTER TABLE hosted_server_starts
    DROP CONSTRAINT hosted_server_starts_cause_check,
    ADD CONSTRAINT hosted_server_starts_cause_check
        CHECK (cause IN ('crash', 'fleet', 'stop', 'owner_stop', 'admin_stop', 'update', 'limits',
                         'no_data', 'timeout', 'link_lost', 'data_unavailable')),
    ADD COLUMN limit_kind  text CHECK (limit_kind IN ('shared_servers', 'no_shared_server',
                                                      'no_shared_room')),
    ADD COLUMN limit_count bigint CHECK (limit_count >= 0),
    ADD CONSTRAINT hosted_server_starts_limit_check CHECK ((limit_kind IS NULL) = (limit_count IS NULL));
