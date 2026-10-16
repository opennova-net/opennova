// The admin's background jobs as GET /api/admin/jobs/overview and /api/admin/jobs
// answer them (apps/novaworld_web/jobs/job_routes.cpp), for the jobs pages'
// tests. NOW is the reply's `at`.

import type { Job, JobKindSummary, JobPage, JobSchedule, JobsOverview } from '../types/jobs';

export const NOW = Date.UTC(2026, 9, 16, 12, 0, 0);

// An ISO time `seconds` before NOW (after it, for a negative one).
export function ago(seconds: number): string {
  return new Date(NOW - seconds * 1000).toISOString().replace(/\.\d{3}Z$/, 'Z');
}

export function job(id: number, overrides: Partial<Job> = {}): Job {
  return {
    id,
    kind: 'machine.describe',
    state: 'done',
    attempts: 1,
    max_attempts: 2,
    last_error: '',
    run_at: ago(120),
    created_at: ago(120),
    started_at: ago(110),
    finished_at: ago(100),
    lease_until: null,
    lease_left_seconds: null,
    running_seconds: null,
    worker_id: 'worker-1a2b3c4d-0',
    dedupe_key: `machine.describe:${id}`,
    schedule_key: null,
    manual: false,
    secret_state: 'none',
    retryable: false,
    cancellable: false,
    ...overrides,
  };
}

export function kindSummary(kind: string, overrides: Partial<JobKindSummary> = {}): JobKindSummary {
  return {
    kind,
    queued: 0,
    due: 0,
    running: 0,
    done_hour: 0,
    done_day: 0,
    failed_hour: 0,
    failed_day: 0,
    failed: 0,
    oldest_wait_seconds: null,
    mean_run_seconds: null,
    ...overrides,
  };
}

export function schedule(kind: string, overrides: Partial<JobSchedule> = {}): JobSchedule {
  return {
    kind,
    interval_seconds: 30,
    at_start: false,
    conditional: false,
    last: job(900, { kind, dedupe_key: kind, schedule_key: `${kind}@59000` }),
    last_finished: job(900, { kind, dedupe_key: kind, schedule_key: `${kind}@59000` }),
    next_at: ago(-20),
    pending: false,
    ...overrides,
  };
}

export function overview(overrides: Partial<JobsOverview> = {}): JobsOverview {
  const perMinute = Array.from({ length: 60 }, (_, i) => ({ at: ago((59 - i) * 60), done: i % 2, failed: 0 }));
  perMinute[59] = { at: ago(0), done: 4, failed: 1 };
  return {
    at: ago(0),
    totals: kindSummary('', {
      queued: 3,
      due: 2,
      running: 2,
      done_hour: 34,
      done_day: 800,
      failed_hour: 1,
      failed_day: 2,
      failed: 5,
      oldest_wait_seconds: 12,
      mean_run_seconds: 1.5,
    }),
    kinds: [
      kindSummary('hosts.prune', { done_hour: 30, done_day: 700, mean_run_seconds: 0.2 }),
      kindSummary('machine.launch', {
        queued: 3,
        due: 2,
        running: 1,
        done_hour: 4,
        done_day: 100,
        failed_hour: 1,
        failed_day: 2,
        failed: 5,
        oldest_wait_seconds: 12,
        mean_run_seconds: 42,
      }),
    ],
    per_minute: perMinute,
    attention: [
      job(77, {
        kind: 'machine.launch',
        state: 'failed',
        attempts: 5,
        max_attempts: 5,
        last_error: 'the launch failed',
        finished_at: ago(600),
        retryable: true,
      }),
      job(78, {
        kind: 'mail.send',
        state: 'failed',
        attempts: 1,
        max_attempts: 1,
        last_error: 'SES refused the address',
        finished_at: ago(3600),
        secret_state: 'dropped',
      }),
    ],
    running: [
      job(80, {
        kind: 'machine.launch',
        state: 'running',
        finished_at: null,
        worker_id: 'worker-1a2b3c4d-1',
        running_seconds: 125,
        lease_until: ago(30),
        lease_left_seconds: -30,
      }),
      job(81, {
        kind: 'billing.sync',
        state: 'running',
        finished_at: null,
        worker_id: 'worker-5e6f7a8b-0',
        running_seconds: 3,
        lease_until: ago(-57),
        lease_left_seconds: 57,
      }),
    ],
    workers: [
      { worker_id: 'worker-5e6f7a8b-0', process: 'worker-5e6f7a8b', job_id: 81, job_kind: 'billing.sync', done: 10, failed: 0, last_active_at: ago(3) },
      { worker_id: 'worker-1a2b3c4d-1', process: 'worker-1a2b3c4d', job_id: 80, job_kind: 'machine.launch', done: 2, failed: 1, last_active_at: ago(125) },
      { worker_id: 'worker-1a2b3c4d-0', process: 'worker-1a2b3c4d', job_id: null, job_kind: null, done: 22, failed: 0, last_active_at: ago(40) },
    ],
    schedules: [
      schedule('hosts.prune'),
      schedule('billing.sync', {
        interval_seconds: 60,
        conditional: true,
        last: null,
        last_finished: null,
        next_at: ago(10),
      }),
      schedule('jobs.prune', { interval_seconds: 3600, pending: true }),
    ],
    controller: { leader: 'controller-0f0f0f0f', elected_at: ago(7200), last_scheduled_at: ago(5) },
    ...overrides,
  };
}

export function jobPage(overrides: Partial<JobPage> = {}): JobPage {
  return {
    jobs: [
      job(90, { kind: 'mail.send', state: 'queued', finished_at: null, started_at: null, worker_id: null, cancellable: true }),
      job(89, { kind: 'machine.launch', state: 'failed', last_error: 'the launch failed', retryable: true }),
      job(88, { kind: 'hosts.prune', schedule_key: 'hosts.prune@59000', dedupe_key: 'hosts.prune' }),
    ],
    total: 120,
    next_before: 88,
    ...overrides,
  };
}
