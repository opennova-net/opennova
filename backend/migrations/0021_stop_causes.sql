-- Who stopped a hosted server (apps/novaworld_common/hosted/model/hosted_repository.h
-- StartCause), so its owner reads it: a start ends 'owner_stop' for its owner's stop and
-- 'admin_stop' for an admin's, the cause set when the stop is asked. 'stop' stays the
-- service's own (its owner's account deleted or no longer active) and every stop before
-- this file. Service policy, not a port.
ALTER TABLE hosted_server_starts
    DROP CONSTRAINT hosted_server_starts_cause_check,
    ADD CONSTRAINT hosted_server_starts_cause_check
        CHECK (cause IN ('crash', 'fleet', 'stop', 'owner_stop', 'admin_stop', 'update', 'limits',
                         'no_data', 'timeout', 'link_lost', 'data_unavailable'));
