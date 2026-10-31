-- The ONE build each running hosted server says it runs, as its machine's agent last read it
-- (apps/novaworld_common/hosted/protocol/agent_protocol.h ServeBuild, opennova-serve's
-- --build-file): its one_build()'s version ("0.0.22"; a staging build's "0.0.22.123") and its
-- commit's short sha ("a1b2c3d"). The public server list shows them beside the server's listing
-- (apps/novaworld_web/lobby_routes.h). '' in both for a report without one: a container that is
-- not running, a server whose start the service rendered no --build-file for, or an agent from
-- before it.
ALTER TABLE fleet_slot_reports ADD COLUMN one_version text NOT NULL DEFAULT '';
ALTER TABLE fleet_slot_reports ADD COLUMN one_commit text NOT NULL DEFAULT '';
