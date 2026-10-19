// The NovaWorld service's background jobs as the admin's jobs routes answer
// them (apps/novaworld_web/jobs/job_routes.h; snake_case). Times are ISO
// 8601 UTC text, durations whole seconds.

export type JobState = 'queued' | 'running' | 'done' | 'failed';

// What a job carries that its payload must not keep: never shown, only
// whether it holds one now, or held one that went when it ended (a job whose
// secret went cannot run again).
export type JobSecretState = 'none' | 'held' | 'dropped';

export interface Job {
  id: number;
  kind: string;
  state: JobState;
  attempts: number;
  max_attempts: number;
  last_error: string;
  run_at: string | null;
  created_at: string | null;
  // Its last claim's.
  started_at: string | null;
  finished_at: string | null;
  // A running job's lease: when it runs out, and the seconds left (negative
  // once it ran out: its worker went away).
  lease_until: string | null;
  lease_left_seconds: number | null;
  running_seconds: number | null;
  worker_id: string | null;
  dedupe_key: string | null;
  schedule_key: string | null;
  // An admin's run now.
  manual: boolean;
  secret_state: JobSecretState;
  retryable: boolean;
  cancellable: boolean;
  // The detail's alone: the JSON its handler reads.
  payload?: unknown;
}

// One kind's jobs now (the totals carry no kind).
export interface JobKindSummary {
  kind: string;
  queued: number;
  due: number;
  running: number;
  done_hour: number;
  done_day: number;
  failed_hour: number;
  failed_day: number;
  failed: number;
  // The oldest due job's wait: the queue's latency.
  oldest_wait_seconds: number | null;
  mean_run_seconds: number | null;
}

// The jobs of one kind that failed the same way in the last day (an admin's
// cancels left out): their failure key is their error with its ids, numbers
// and times taken out.
export interface JobFailureGroup {
  kind: string;
  key: string;
  count: number;
  // How many a retry takes: the rest carried a secret that went as they ended.
  retryable: number;
  last_job_id: number;
  // The newest one's own error, and when it failed.
  last_error: string;
  last_at: string | null;
}

// A worker process as its threads' claims show it (those holding a job, or
// done with one in the last hour).
export interface JobWorker {
  process: string;
  threads: number;
  busy: number;
  // Its running jobs whose lease ran out (the process went away), and the
  // least lease left of the others.
  lapsed: number;
  lease_left_seconds: number | null;
  done: number;
  failed: number;
  last_active_at: string | null;
}

// A periodic kind's schedule.
export interface JobSchedule {
  kind: string;
  interval_seconds: number;
  at_start: boolean;
  // Its run only while there is work for it (the billing sync's events, or
  // prices not checked under the worker's lists).
  conditional: boolean;
  last: Job | null;
  last_finished: Job | null;
  next_at: string;
  // One of its kind waits or runs.
  pending: boolean;
}

// The fleet process that leads (no program schedules on its behalf: each
// schedules its own kinds).
export interface JobsFleet {
  leader: string | null;
  elected_at: string | null;
}

export interface JobsOverview {
  at: string;
  totals: JobKindSummary;
  kinds: JobKindSummary[];
  // The newest failure groups, and how many jobs every group counts.
  attention: JobFailureGroup[];
  attention_total: number;
  workers: JobWorker[];
  schedules: JobSchedule[];
  // The newest job a schedule enqueued.
  last_scheduled_at: string | null;
  fleet: JobsFleet;
}

// What a failure group's retry did with each of its jobs.
export interface JobGroupRetry {
  retried: number;
  secret_dropped: number;
  already_pending: number;
  changed: number;
}

export interface JobQuery {
  state?: JobState;
  kind?: string;
  // Unix seconds, on when a job was created; `to` excluded.
  from?: number;
  to?: number;
  // Within its dedupe or schedule key.
  q?: string;
  // A failure group's key, exact.
  failure?: string;
  // The page after the one that ended at this id.
  before?: number;
  limit?: number;
}

export interface JobPage {
  jobs: Job[];
  total: number;
  next_before: number | null;
}
