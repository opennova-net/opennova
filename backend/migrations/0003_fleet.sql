-- The hosted servers' fleet (apps/novaworld_server/hosted/fleet_repository.h):
-- the machines the service launches, the slot each live server holds on one,
-- and what each machine's agent (opennova-hostd) last reported of its slots.
-- Service policy, not a port.

-- A machine: an EC2 virtual machine (provider 'ec2') or a development stand-in
-- (provider 'local'), launched from the launch template of its kind. A shared
-- machine packs up to `slots` servers of any account; an own machine runs one,
-- the server of the own-machine add-on it is reserved_for, and ends when that
-- server stops (hosted/placement.h). Its lifecycle:
--
--   launching   the row and its one-time bootstrap token are made; the cloud
--               is asked for the machine (cloud_id empty until it answers)
--   booting     the cloud runs it (cloud_id, private_address set); its agent
--               has not reported yet
--   ready       its agent reports (last_report_at); servers land on it
--   draining    its servers move off it, one at a time; nothing new lands
--   ending      the cloud is asked to end it
--   ended       gone (ended_at, end_reason)
--   failed      gone after a fault (a launch the cloud refused, a boot that
--               never reported)
--   lost        its agent stopped reporting (last_report_at older than a
--               minute): the cloud is asked whether it still runs
--
-- The agent's credentials are SHA-256 digests (64 lower-case hex digits),
-- never the values: bootstrap_token_hash is the one-time token the machine's
-- user-data carries, until it is redeemed (then '') or lapses at
-- bootstrap_expires_at; agent_secret_hash the long-lived secret the bootstrap
-- hands the agent for its syncs ('' until then). An ended or failed machine
-- keeps neither. serve_image is the image the machine's agent was copied out
-- of (the bootstrap reply names it); template_version the launch template and
-- its version the cloud launched it from, so a fleet roll can tell an old
-- machine from a new one.
CREATE TABLE fleet_machines (
    id                   bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    provider             text NOT NULL CHECK (provider <> ''),
    kind                 text NOT NULL CHECK (kind IN ('shared', 'own')),
    state                text NOT NULL DEFAULT 'launching'
        CHECK (state IN ('launching', 'booting', 'ready', 'draining', 'ending', 'ended',
                         'failed', 'lost')),
    cloud_id             text NOT NULL DEFAULT '',
    instance_type        text NOT NULL DEFAULT '',
    slots                bigint NOT NULL CHECK (slots BETWEEN 1 AND 32),
    private_address      text NOT NULL DEFAULT '',
    public_address       text NOT NULL DEFAULT '',
    bootstrap_token_hash text NOT NULL DEFAULT '',
    bootstrap_expires_at timestamptz,
    agent_secret_hash    text NOT NULL DEFAULT '',
    agent_version        text NOT NULL DEFAULT '',
    template_version     text NOT NULL DEFAULT '',
    serve_image          text NOT NULL DEFAULT '',
    reserved_for         bigint REFERENCES hosted_servers(id) ON DELETE SET NULL,
    last_report_at       timestamptz,
    launched_at          timestamptz NOT NULL DEFAULT now(),
    ready_at             timestamptz,
    ended_at             timestamptz,
    end_reason           text NOT NULL DEFAULT ''
);
CREATE INDEX idx_fleet_machines_state ON fleet_machines(state);
-- One row per cloud machine: a listing's machine maps back to its row.
CREATE UNIQUE INDEX idx_fleet_machines_cloud_id ON fleet_machines(provider, cloud_id)
    WHERE cloud_id <> '';

-- A hosted server's place in the fleet. machine_ref is the machine it runs on
-- and slot its slot there (hosted/slots.h: slot k is the game's UDP port
-- 32768 + k on a cloud machine, game_port the one the server was rendered
-- with), held while the server is live: two live servers never hold one slot
-- of a machine (idx_hosted_servers_live_slot), and an ended server's row keeps
-- where it last ran. generation counts the server's starts: its container is
-- srv-<id>-g<generation> (hosted/agent_protocol.h), a new name at every start.
-- machine_id holds that container's name and machine_address the machine's
-- private address, where the service's admin link connects.
ALTER TABLE hosted_servers
    ADD COLUMN machine_ref bigint REFERENCES fleet_machines(id) ON DELETE SET NULL,
    ADD COLUMN slot        bigint CHECK (slot IS NULL OR (slot >= 0 AND slot < 32)),
    ADD COLUMN game_port   bigint NOT NULL DEFAULT 0 CHECK (game_port BETWEEN 0 AND 65535),
    ADD COLUMN generation  bigint NOT NULL DEFAULT 0;
CREATE UNIQUE INDEX idx_hosted_servers_live_slot ON hosted_servers(machine_ref, slot)
    WHERE machine_ref IS NOT NULL AND state NOT IN ('stopped', 'failed');
CREATE INDEX idx_hosted_servers_machine_ref ON hosted_servers(machine_ref);

-- What a machine's agent last reported of each slot it runs a container on
-- (POST /hosted/machines/<id>/sync, hosted/agent_route.h): the container, its
-- state ('starting', 'running', 'exited'), how it exited, when it started, its
-- CPU (percent of one CPU) and memory, and the slot's egress (every byte the
-- slot's user sent since the machine booted: the stats take the differences).
-- Each sync replaces the machine's rows: a slot it no longer reports goes.
CREATE TABLE fleet_slot_reports (
    machine_id   bigint NOT NULL REFERENCES fleet_machines(id) ON DELETE CASCADE,
    slot         bigint NOT NULL CHECK (slot >= 0 AND slot < 32),
    container    text NOT NULL,
    state        text NOT NULL CHECK (state IN ('starting', 'running', 'exited')),
    exit_code    bigint,
    oom          boolean NOT NULL DEFAULT false,
    started_at   timestamptz,
    cpu_percent  double precision NOT NULL DEFAULT 0,
    memory_bytes bigint NOT NULL DEFAULT 0,
    egress_bytes bigint NOT NULL DEFAULT 0,
    reported_at  timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (machine_id, slot)
);
