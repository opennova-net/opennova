-- The launch backoff grows (apps/novaworld_common/hosted/supervisor.h): each
-- kind's failed launches in a row (refused by the cloud, or a machine of it
-- that never booted), counted once per backoff (a failure that lands while one
-- holds came from a launch made before it began, so it changes nothing). The
-- n-th holds launches of the kind for the base backoff doubled n - 1 times, to
-- a cap; a machine of the kind that boots (its agent's first sync) sets the
-- count back to 0 and ends the backoff. Service policy, not a port.
ALTER TABLE fleet_launch_backoff
    ADD COLUMN failures bigint NOT NULL DEFAULT 0 CHECK (failures >= 0);
