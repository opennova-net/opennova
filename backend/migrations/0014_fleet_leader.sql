-- The fleet program's leadership (apps/novaworld_fleet/election.h): the
-- controller's row (0008_controller_state.sql) under the fleet program's
-- name, the controller program now being the fleet program (ADR 0065). The
-- row moves as it is, its generation with it: a fleet leader moves the same
-- generation on, so it stays monotonic across the rename, and a console
-- command an earlier leader claimed (hosted_console_commands.
-- claimed_generation) is still told apart. Service policy, not a port.
ALTER TABLE controller_leader RENAME TO fleet_leader;
ALTER TABLE fleet_leader RENAME CONSTRAINT controller_leader_pkey TO fleet_leader_pkey;
ALTER TABLE fleet_leader RENAME CONSTRAINT controller_leader_id_check TO fleet_leader_id_check;
