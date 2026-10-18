-- What a machine's agent says of itself at every sync, and the supervisor's brake on a
-- fleet that goes lost at once (apps/novaworld_common/hosted/fleet_repository.h, ADR 0065).
-- Service policy, not a port.

-- The envelope every sync opens with (hosted/agent_protocol.h AgentEnvelope), recorded
-- whether or not its protocol is the service's: agent_protocol the protocol the agent
-- speaks (NULL before its first sync), agent_image the image its ECS task runs ('' off
-- ECS), and protocol_mismatch_since when its syncs began to speak another protocol than
-- the service's (NULL while they match). A mismatched machine is held: heard, acted on
-- in nothing, and drained only once the mismatch has lasted 10 minutes (agent_route.h),
-- so a hostd rolled before the fleet program, or after it, keeps its servers running.
-- lost_held_since_ms is the supervisor's (on its clock, as seen_since_ms): since when the
-- mass-lost brake holds this lost machine from its end (0: not held).
ALTER TABLE fleet_machines
    ADD COLUMN agent_protocol          bigint,
    ADD COLUMN agent_image             text NOT NULL DEFAULT '',
    ADD COLUMN protocol_mismatch_since timestamptz,
    ADD COLUMN lost_held_since_ms      bigint NOT NULL DEFAULT 0;
