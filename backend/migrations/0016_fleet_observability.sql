-- What the fleet records of itself (apps/novaworld_common/hosted/fleet_repository.h,
-- hosted_repository.h, stats_repository.h): every move of a machine, each of a
-- hosted server's runs, what each machine's agent says of the machine (its image,
-- its own log stream, its use, the trees it holds) and of each slot (its network,
-- its server's tick health, what a starting container waits for), and the
-- machines' use by the minute and by the hour. Service policy, not a port.

-- Every move of a machine, written in the same statement as the move
-- (fleet_repository.h): from_state is null for the row's making, and a row whose
-- to_state is its from_state notes the agent's new log stream ("log stream
-- <name>"). Kept with the machine's row (14 days past its end).
CREATE TABLE fleet_machine_events (
    id         bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    machine_id bigint NOT NULL REFERENCES fleet_machines(id) ON DELETE CASCADE,
    from_state text,
    to_state   text NOT NULL,
    detail     text NOT NULL DEFAULT '',
    at         timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX idx_fleet_machine_events_machine ON fleet_machine_events(machine_id, id);
CREATE INDEX idx_fleet_machine_events_at ON fleet_machine_events(at);

-- What the machine's agent last said of the machine (agent_protocol.h): its
-- image (an AMI id, '' off EC2), the agent's own log stream ('' off ECS), and its
-- use as the last report read it (stats_at; all null before one): the CPU busy
-- of the whole machine (0..100), its memory, the trees' filesystem, and every
-- interface's bytes but loopback's since the machine booted.
ALTER TABLE fleet_machines
    ADD COLUMN image_id           text NOT NULL DEFAULT '',
    ADD COLUMN agent_log_stream   text NOT NULL DEFAULT '',
    ADD COLUMN cpu_percent        double precision,
    ADD COLUMN memory_used_bytes  bigint,
    ADD COLUMN memory_total_bytes bigint,
    ADD COLUMN disk_free_bytes    bigint,
    ADD COLUMN disk_total_bytes   bigint,
    ADD COLUMN net_in_bytes       bigint,
    ADD COLUMN net_out_bytes      bigint,
    ADD COLUMN stats_at           timestamptz;

-- Every tree a machine's agent holds, as its last report said (each report
-- replaces the machine's rows): fetching (a failure with a try left among
-- them), ready, or failed for good; its bytes on disk (the fetch's so far), the
-- fetches made and the last failure's reason.
CREATE TABLE fleet_machine_trees (
    machine_id  bigint NOT NULL REFERENCES fleet_machines(id) ON DELETE CASCADE,
    tree_id     bigint NOT NULL,
    state       text NOT NULL CHECK (state IN ('fetching', 'ready', 'failed')),
    bytes       bigint NOT NULL DEFAULT 0 CHECK (bytes >= 0),
    tries       bigint NOT NULL DEFAULT 0 CHECK (tries >= 0),
    error       text NOT NULL DEFAULT '',
    reported_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (machine_id, tree_id)
);

-- A slot's report gains what a starting container waits for ('image', 'trees'
-- or ''), the bytes that reached the slot's ports since the machine booted, and
-- its running server's last minute of ticks (null without a fresh one).
ALTER TABLE fleet_slot_reports
    ADD COLUMN waiting      text NOT NULL DEFAULT '' CHECK (waiting IN ('', 'image', 'trees')),
    ADD COLUMN net_in_bytes bigint NOT NULL DEFAULT 0,
    ADD COLUMN tick_p50_ms  double precision,
    ADD COLUMN tick_p99_ms  double precision,
    ADD COLUMN tick_max_ms  double precision,
    ADD COLUMN late_ticks   bigint;

-- Each run of a hosted server: a hosted_servers row is one owner's request, a
-- generation one of its starts (its container srv-<id>-g<generation>, its log
-- stream <id>/g<generation>). Made when its slot is assigned (placed_at, the
-- machine and the slot), its image, trees and log stream written when the start
-- renders, running_at when its agent first reports it running, and ended with
-- why: the cause (crash: the server exited or never came up, its exit code, OOM
-- flag and its exit line or why it never started kept; timeout: it did not
-- start or list in time; fleet: its machine's or NovaWorld's cause, a move;
-- stop: an owner's, an admin's or the service's stop; update: the owner's
-- update to the current data; limits: its owner's entitlement no longer covers
-- it; no_data: its data set has none; link_lost: its admin link stopped
-- answering). The restarts the stats count are its crash and timeout ends.
-- machine_id stays a plain id: the run outlives its machine's row.
CREATE TABLE hosted_server_starts (
    server_id         bigint NOT NULL REFERENCES hosted_servers(id) ON DELETE CASCADE,
    generation        bigint NOT NULL,
    machine_id        bigint NOT NULL,
    slot              bigint NOT NULL CHECK (slot >= 0 AND slot < 32),
    serve_image       text NOT NULL DEFAULT '',
    base_tree_id      bigint,
    expansion_tree_id bigint,
    placed_at         timestamptz NOT NULL DEFAULT now(),
    running_at        timestamptz,
    ended_at          timestamptz,
    exit_code         bigint,
    oom               boolean NOT NULL DEFAULT false,
    reason            text NOT NULL DEFAULT '',
    cause             text CHECK (cause IN ('crash', 'fleet', 'stop', 'update', 'limits',
                                            'no_data', 'timeout', 'link_lost')),
    log_stream        text NOT NULL DEFAULT '',
    PRIMARY KEY (server_id, generation)
);
CREATE INDEX idx_hosted_server_starts_ended ON hosted_server_starts(ended_at);
CREATE INDEX idx_hosted_server_starts_open ON hosted_server_starts(server_id)
    WHERE ended_at IS NULL;

-- A server's minute gains its slot's network as counters (bytes in and out
-- since the server's first sample on the slot: a reader takes the differences,
-- and a count below the last starts a new run of them) and its last minute of
-- ticks; its hour the peak memory, the bytes in and out, the worst minute's
-- p99 and the late ticks. The hours are kept 400 days.
ALTER TABLE hosted_server_samples
    ADD COLUMN net_in_bytes  bigint,
    ADD COLUMN net_out_bytes bigint,
    ADD COLUMN tick_p50_ms   double precision,
    ADD COLUMN tick_p99_ms   double precision,
    ADD COLUMN tick_max_ms   double precision,
    ADD COLUMN late_ticks    bigint;
ALTER TABLE hosted_server_hourly
    ADD COLUMN peak_memory_bytes bigint,
    ADD COLUMN net_in_bytes      bigint NOT NULL DEFAULT 0,
    ADD COLUMN net_out_bytes     bigint NOT NULL DEFAULT 0,
    ADD COLUMN tick_p99_max_ms   double precision,
    ADD COLUMN late_ticks        bigint NOT NULL DEFAULT 0;

-- A machine's minute, from its agent's last report (its fleet_machines
-- columns): its use and how many live servers it held; the network as the
-- agent's counters (since the machine booted). Kept 48 hours. Its hour, rolled
-- up from the minutes by stats.prune: the CPU's mean and peak, the memory's
-- peak, the trees' filesystem's least room, the bytes in and out. Kept 400
-- days. machine_id is a plain id: the history outlives the machine's row.
CREATE TABLE fleet_machine_samples (
    machine_id         bigint NOT NULL,
    at                 timestamptz NOT NULL,
    cpu_percent        double precision,
    memory_used_bytes  bigint,
    memory_total_bytes bigint,
    disk_free_bytes    bigint,
    net_in_bytes       bigint,
    net_out_bytes      bigint,
    servers            bigint NOT NULL DEFAULT 0,
    PRIMARY KEY (machine_id, at)
);
CREATE INDEX idx_fleet_machine_samples_at ON fleet_machine_samples(at);
CREATE TABLE fleet_machine_hourly (
    machine_id          bigint NOT NULL,
    hour                timestamptz NOT NULL,
    cpu_avg             double precision,
    cpu_max             double precision,
    memory_peak_bytes   bigint,
    disk_free_min_bytes bigint,
    net_in_bytes        bigint NOT NULL DEFAULT 0,
    net_out_bytes       bigint NOT NULL DEFAULT 0,
    PRIMARY KEY (machine_id, hour)
);
CREATE INDEX idx_fleet_machine_hourly_hour ON fleet_machine_hourly(hour);

-- A machine's egress by the month outlives the machine's row (14 days past its
-- end) to its own 13 months: machine_id is in its key, so the row keeps a plain
-- id rather than a reference that would take it with the machine.
ALTER TABLE fleet_machine_egress DROP CONSTRAINT fleet_machine_egress_machine_id_fkey;
