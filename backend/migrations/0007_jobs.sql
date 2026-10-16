-- The service's background jobs (apps/novaworld_server/jobs/job_queue.h): one
-- queue in the database for the work no request or pass waits on (the fleet's
-- cloud calls, the billing sync, the outbound mail, the prunes), so a job is
-- written in the same transaction as the change that causes it and survives a
-- restart of whatever wrote it. Service policy, not a port.

-- A job: what to run (kind, payload), its state, when it may run next, how
-- often it ran, the worker holding it and until when, and why it last failed.
--
--   queued    waiting for run_at; a worker claims it (FOR UPDATE SKIP LOCKED)
--   running   a worker holds it until lease_until, which it extends while it
--             runs; an expired lease (its worker went away) puts it back to
--             queued, or failed once attempts reaches max_attempts
--   done      it ran
--   failed    it failed max_attempts times, or failed for good
--
-- secret holds what the payload must not keep (a machine's one-time bootstrap
-- token, a mail's body with its reset link): set at enqueue, cleared when the
-- job ends. dedupe_key is unique among the queued and running jobs (a machine
-- has at most one describe waiting); schedule_key is unique for good, the
-- periodic jobs' kind and time bucket. Finished jobs are pruned 14 days after
-- they end (the jobs.prune job).
CREATE TABLE jobs (
    id           bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    kind         text NOT NULL CHECK (kind <> ''),
    payload      jsonb NOT NULL DEFAULT '{}',
    secret       text,
    state        text NOT NULL DEFAULT 'queued'
        CHECK (state IN ('queued', 'running', 'done', 'failed')),
    run_at       timestamptz NOT NULL DEFAULT now(),
    attempts     bigint NOT NULL DEFAULT 0 CHECK (attempts >= 0),
    max_attempts bigint NOT NULL DEFAULT 5 CHECK (max_attempts >= 1),
    lease_until  timestamptz,
    worker_id    text,
    last_error   text NOT NULL DEFAULT '',
    dedupe_key   text CHECK (dedupe_key IS NULL OR dedupe_key <> ''),
    schedule_key text UNIQUE CHECK (schedule_key IS NULL OR schedule_key <> ''),
    created_at   timestamptz NOT NULL DEFAULT now(),
    started_at   timestamptz,
    finished_at  timestamptz
);
CREATE UNIQUE INDEX idx_jobs_dedupe ON jobs(dedupe_key)
    WHERE dedupe_key IS NOT NULL AND state IN ('queued', 'running');
CREATE INDEX idx_jobs_queued ON jobs(run_at, id) WHERE state = 'queued';
CREATE INDEX idx_jobs_running ON jobs(lease_until) WHERE state = 'running';
CREATE INDEX idx_jobs_finished ON jobs(finished_at) WHERE state IN ('done', 'failed');
CREATE INDEX idx_jobs_kind_state ON jobs(kind, state);

-- The fleet's machines on the jobs (apps/novaworld_server/hosted/fleet_jobs.h):
-- the supervisor's pass never calls the cloud; it enqueues machine.launch,
-- machine.describe and machine.terminate and reads their results here.
--
-- cloud_state is what the last describe (or the sweep's) said of the machine
-- ('' until one did, and again from when it went lost): pending, running, gone
-- or unknown (the cloud did not answer); described_at when. launched_for is the
-- server whose placement launched it; launch_refused a launch the cloud
-- refused that the supervisor has not counted yet (its launch backoff, the
-- heartbeat's launch_failures, the server's event). launch_subnet is the subnet
-- its launch last called the cloud in, written before that call ('' until one
-- did): a launch run again calls there first, under the same client token, so
-- EC2 answers it with the machine the earlier call launched, however late EC2
-- lists that machine.
--
-- What the supervisor remembers of a live machine between passes, here so a
-- new leader resumes it (supervisor.h): seen_state and seen_since_ms, the
-- state its pass last saw and since when on its clock (Unix milliseconds);
-- idle_since_ms, since when a shared machine has held no server (0: it holds
-- one); drain_moving, the server its drain moved last (0: none); was_draining,
-- a lost machine that was draining; describe_asked_ms, when the pass last
-- asked for a describe.
ALTER TABLE fleet_machines
    ADD COLUMN cloud_state       text NOT NULL DEFAULT ''
        CHECK (cloud_state IN ('', 'pending', 'running', 'gone', 'unknown')),
    ADD COLUMN described_at      timestamptz,
    ADD COLUMN launched_for      bigint REFERENCES hosted_servers(id) ON DELETE SET NULL,
    ADD COLUMN launch_refused    boolean NOT NULL DEFAULT false,
    ADD COLUMN launch_subnet     text NOT NULL DEFAULT '',
    ADD COLUMN seen_state        text NOT NULL DEFAULT '',
    ADD COLUMN seen_since_ms     bigint NOT NULL DEFAULT 0,
    ADD COLUMN idle_since_ms     bigint NOT NULL DEFAULT 0,
    ADD COLUMN drain_moving      bigint NOT NULL DEFAULT 0,
    ADD COLUMN was_draining      boolean NOT NULL DEFAULT false,
    ADD COLUMN describe_asked_ms bigint NOT NULL DEFAULT 0;
CREATE INDEX idx_fleet_machines_launch_refused ON fleet_machines(id) WHERE launch_refused;

-- After a launch of a kind the cloud refused (or a machine of it that never
-- booted), no launch of that kind before not_before_ms, on the supervisor's
-- clock (Unix milliseconds).
CREATE TABLE fleet_launch_backoff (
    kind          text PRIMARY KEY CHECK (kind IN ('shared', 'own')),
    not_before_ms bigint NOT NULL
);

-- The billing sync's state (apps/novaworld_server/billing/billing_sync.h),
-- one row, written by the billing jobs wherever they run and read by the
-- routes and the heartbeat: the configured prices as Stripe last answered
-- them (prices_config names the price lists they were checked under, so a
-- changed configuration reads unchecked until it is checked again), the last
-- full reconcile and what it found, and when the billing sync first ran. The
-- lists are JSON text.
CREATE TABLE billing_sync_state (
    id                     bigint PRIMARY KEY CHECK (id = 1),
    prices_config          text NOT NULL DEFAULT '',
    prices_checked         boolean NOT NULL DEFAULT false,
    prices_ok              boolean NOT NULL DEFAULT false,
    prices_error           text NOT NULL DEFAULT '',
    prices                 text NOT NULL DEFAULT '{}',
    price_check_after_s    bigint NOT NULL DEFAULT 0,
    last_reconcile_s       bigint NOT NULL DEFAULT 0,
    last_reconcile_error   text NOT NULL DEFAULT '',
    orphans                text NOT NULL DEFAULT '[]',
    duplicates             text NOT NULL DEFAULT '[]',
    unknown_prices         text NOT NULL DEFAULT '[]',
    started_s              bigint NOT NULL DEFAULT 0
);
INSERT INTO billing_sync_state (id) VALUES (1);

-- A customer Stripe could not be asked about waits before the billing sync
-- asks again: until next_s (Unix seconds), the wait doubling (delay_s) up to
-- its cap. Gone once the customer syncs.
CREATE TABLE billing_sync_backoff (
    stripe_customer_id text PRIMARY KEY,
    next_s             bigint NOT NULL,
    delay_s            bigint NOT NULL
);
