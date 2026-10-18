-- What each program's tasks use (apps/novaworld_common/program_usage.h): every
-- program's task samples itself once a minute and writes its own row, kept 48
-- hours, rolled up by the hour (the worker's stats.prune) and kept 400 days.
-- Keyed by the program and its task (an ECS task's id; a local container's
-- hostname), with no foreign key: a task's history outlives the task. The admin's
-- usage view reads them (apps/novaworld_web/infra/usage_routes.h). Service
-- policy, not a port.

-- A task's minute: `at` the minute it was taken (UTC, seconds 0); its CPU over
-- the minute before, in percent of its CPU limit (0..100; null for a task's
-- first minute, which has no reading before it), that limit in CPUs; its memory
-- as `docker stats` counts it and the most it may use; and its network
-- namespace's bytes in and out since it came up (cumulative counters: a reader
-- takes the rate between two minutes, and a smaller count is one that started
-- over). Each null when its file did not read.
CREATE TABLE program_samples (
    program            text NOT NULL,
    task_id            text NOT NULL,
    at                 timestamptz NOT NULL,
    cpu_percent        real,
    cpu_limit          real,
    memory_used_bytes  bigint,
    memory_limit_bytes bigint,
    net_in_bytes       bigint,
    net_out_bytes      bigint,
    PRIMARY KEY (program, task_id, at)
);
CREATE INDEX idx_program_samples_at ON program_samples(at);

-- A task's hour (UTC), from its minutes: their mean and peak CPU, its peak
-- memory and the bytes it took in and sent in that hour (a counter that started
-- over counts from 0). Rewritten while the hour's minutes are kept.
CREATE TABLE program_hourly (
    program           text NOT NULL,
    task_id           text NOT NULL,
    hour              timestamptz NOT NULL,
    cpu_avg           real,
    cpu_max           real,
    memory_peak_bytes bigint,
    net_in_bytes      bigint NOT NULL DEFAULT 0,
    net_out_bytes     bigint NOT NULL DEFAULT 0,
    PRIMARY KEY (program, task_id, hour)
);
CREATE INDEX idx_program_hourly_hour ON program_hourly(hour);
