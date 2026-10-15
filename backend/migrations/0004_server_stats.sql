-- The hosted servers' stats (apps/novaworld_server/hosted/stats_sampler.h,
-- stats_repository.h): one sample of each live server a minute, rolled up by
-- the hour, and each machine's egress by the calendar month. Counts alone: no
-- player's name is stored (unique players are counted in the service's memory,
-- an hour at a time). Service policy, not a port.

-- A server's minute: `at` the minute it was taken (UTC, seconds 0), its
-- state then, its listing's players, slots and mission (null while it is not
-- listed), its admin link's scene ('game', 'menus' or 'unknown'; null with no
-- link) and whether the link was up, its container's CPU (percent of the
-- slot's CPUs, 0..100) and memory as its machine's agent last reported them
-- (null with no report), the bytes its slot sent this minute (the agent's
-- per-slot counter's difference; a counter that started over counts from 0),
-- and whether this minute saw a new mission start in the game scene (the
-- rotation's missions, counted from the changes the samples see: an
-- approximation). Kept 48 hours.
CREATE TABLE hosted_server_samples (
    server_id       bigint NOT NULL REFERENCES hosted_servers(id) ON DELETE CASCADE,
    at              timestamptz NOT NULL,
    state           text NOT NULL,
    players         bigint,
    max_players     bigint,
    mission         text,
    scene           text,
    link_up         boolean NOT NULL DEFAULT false,
    cpu_percent     double precision,
    memory_bytes    bigint,
    egress_bytes    bigint NOT NULL DEFAULT 0 CHECK (egress_bytes >= 0),
    mission_started boolean NOT NULL DEFAULT false,
    PRIMARY KEY (server_id, at)
);
CREATE INDEX idx_hosted_server_samples_at ON hosted_server_samples(at);

-- A server's hour (UTC), from its samples: the minutes it was live and
-- running, its player-minutes, its peak players and slots, the distinct
-- players its listing's roster named (counted, never stored), the missions
-- started, the bytes sent and the mean CPU. Rewritten from the samples every
-- minute while the hour runs. Kept 90 days.
CREATE TABLE hosted_server_hourly (
    server_id       bigint NOT NULL REFERENCES hosted_servers(id) ON DELETE CASCADE,
    hour            timestamptz NOT NULL,
    live_minutes    bigint NOT NULL DEFAULT 0,
    running_minutes bigint NOT NULL DEFAULT 0,
    player_minutes  bigint NOT NULL DEFAULT 0,
    peak_players    bigint NOT NULL DEFAULT 0,
    max_players     bigint NOT NULL DEFAULT 0,
    unique_players  bigint NOT NULL DEFAULT 0,
    missions        bigint NOT NULL DEFAULT 0,
    egress_bytes    bigint NOT NULL DEFAULT 0,
    avg_cpu         double precision,
    PRIMARY KEY (server_id, hour)
);
CREATE INDEX idx_hosted_server_hourly_hour ON hosted_server_hourly(hour);

-- A machine's egress by calendar month (UTC): every byte its slots sent, as
-- the samples count it (the admin's fleet view).
CREATE TABLE fleet_machine_egress (
    machine_id   bigint NOT NULL REFERENCES fleet_machines(id) ON DELETE CASCADE,
    month        date NOT NULL,
    egress_bytes bigint NOT NULL DEFAULT 0 CHECK (egress_bytes >= 0),
    PRIMARY KEY (machine_id, month)
);
