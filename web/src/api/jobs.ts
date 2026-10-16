// The admin's background jobs (/api/admin/jobs*,
// apps/novaworld_web/jobs/job_routes.h): the overview, a page of jobs, one
// job, and the actions on them. Every call answers an AdminResult (admin.ts),
// a refusal's code mapped to a message there.

import { adminClient } from './client';
import { adminCall, type AdminResult } from './admin';
import type {
  Job,
  JobKindSummary,
  JobMinute,
  JobPage,
  JobQuery,
  JobSchedule,
  JobSecretState,
  JobState,
  JobWorker,
  JobsOverview,
} from '../types/jobs';

type Fields = Record<string, unknown>;

const isObject = (v: unknown): v is Fields => typeof v === 'object' && v !== null && !Array.isArray(v);
const isCount = (v: unknown): v is number => Number.isInteger(v) && (v as number) >= 0;
const text = (v: unknown): string => (typeof v === 'string' ? v : '');
const textOrNull = (v: unknown): string | null => (typeof v === 'string' && v !== '' ? v : null);
const numberOrNull = (v: unknown): number | null => (typeof v === 'number' && Number.isFinite(v) ? v : null);

const STATES: readonly JobState[] = ['queued', 'running', 'done', 'failed'];
const SECRETS: readonly JobSecretState[] = ['none', 'held', 'dropped'];

export function pickJob(value: unknown): Job | null {
  if (!isObject(value) || !isCount(value.id) || typeof value.kind !== 'string') return null;
  if (!STATES.includes(value.state as JobState)) return null;
  const job: Job = {
    id: value.id,
    kind: value.kind,
    state: value.state as JobState,
    attempts: isCount(value.attempts) ? value.attempts : 0,
    max_attempts: isCount(value.max_attempts) ? value.max_attempts : 0,
    last_error: text(value.last_error),
    run_at: textOrNull(value.run_at),
    created_at: textOrNull(value.created_at),
    started_at: textOrNull(value.started_at),
    finished_at: textOrNull(value.finished_at),
    lease_until: textOrNull(value.lease_until),
    lease_left_seconds: numberOrNull(value.lease_left_seconds),
    running_seconds: numberOrNull(value.running_seconds),
    worker_id: textOrNull(value.worker_id),
    dedupe_key: textOrNull(value.dedupe_key),
    schedule_key: textOrNull(value.schedule_key),
    manual: value.manual === true,
    secret_state: SECRETS.includes(value.secret_state as JobSecretState) ? (value.secret_state as JobSecretState) : 'none',
    retryable: value.retryable === true,
    cancellable: value.cancellable === true,
  };
  if ('payload' in value) job.payload = value.payload;
  return job;
}

function pickJobs(value: unknown): Job[] | null {
  if (!Array.isArray(value)) return null;
  const jobs = value.map(pickJob);
  return jobs.some((j) => j === null) ? null : (jobs as Job[]);
}

function pickKind(value: unknown): JobKindSummary | null {
  if (!isObject(value)) return null;
  const count = (key: string) => (isCount(value[key]) ? (value[key] as number) : 0);
  return {
    kind: text(value.kind),
    queued: count('queued'),
    due: count('due'),
    running: count('running'),
    done_hour: count('done_hour'),
    done_day: count('done_day'),
    failed_hour: count('failed_hour'),
    failed_day: count('failed_day'),
    failed: count('failed'),
    oldest_wait_seconds: numberOrNull(value.oldest_wait_seconds),
    mean_run_seconds: numberOrNull(value.mean_run_seconds),
  };
}

function pickMinute(value: unknown): JobMinute | null {
  if (!isObject(value) || typeof value.at !== 'string') return null;
  return { at: value.at, done: isCount(value.done) ? value.done : 0, failed: isCount(value.failed) ? value.failed : 0 };
}

function pickWorker(value: unknown): JobWorker | null {
  if (!isObject(value) || typeof value.worker_id !== 'string') return null;
  return {
    worker_id: value.worker_id,
    process: text(value.process) || value.worker_id,
    job_id: isCount(value.job_id) ? value.job_id : null,
    job_kind: textOrNull(value.job_kind),
    done: isCount(value.done) ? value.done : 0,
    failed: isCount(value.failed) ? value.failed : 0,
    last_active_at: textOrNull(value.last_active_at),
  };
}

function pickSchedule(value: unknown): JobSchedule | null {
  if (!isObject(value) || typeof value.kind !== 'string' || !isCount(value.interval_seconds)) return null;
  return {
    kind: value.kind,
    interval_seconds: value.interval_seconds,
    at_start: value.at_start === true,
    conditional: value.conditional === true,
    last: value.last == null ? null : pickJob(value.last),
    last_finished: value.last_finished == null ? null : pickJob(value.last_finished),
    next_at: text(value.next_at),
    pending: value.pending === true,
  };
}

// Every member of `list` read by `pick`; null when it is no list or one does not read.
function every<T>(list: unknown, pick: (v: unknown) => T | null): T[] | null {
  if (!Array.isArray(list)) return null;
  const out = list.map(pick);
  return out.some((v) => v === null) ? null : (out as T[]);
}

export function pickOverview(data: unknown): JobsOverview | null {
  if (!isObject(data) || typeof data.at !== 'string') return null;
  const totals = pickKind(data.totals);
  const kinds = every(data.kinds, pickKind);
  const perMinute = every(data.per_minute, pickMinute);
  const attention = pickJobs(data.attention);
  const running = pickJobs(data.running);
  const workers = every(data.workers, pickWorker);
  const schedules = every(data.schedules, pickSchedule);
  if (!totals || !kinds || !perMinute || !attention || !running || !workers || !schedules) return null;
  const controller = isObject(data.controller) ? data.controller : {};
  return {
    at: data.at,
    totals,
    kinds,
    per_minute: perMinute,
    attention,
    running,
    workers,
    schedules,
    controller: {
      leader: textOrNull(controller.leader),
      elected_at: textOrNull(controller.elected_at),
      last_scheduled_at: textOrNull(controller.last_scheduled_at),
    },
  };
}

function pickPage(data: unknown): JobPage | null {
  if (!isObject(data)) return null;
  const jobs = pickJobs(data.jobs);
  if (!jobs) return null;
  return {
    jobs,
    total: isCount(data.total) ? data.total : jobs.length,
    next_before: isCount(data.next_before) ? data.next_before : null,
  };
}

function pickJobReply(data: unknown): Job | null {
  return isObject(data) ? pickJob(data.job) : null;
}

// GET /api/admin/jobs/overview.
export function fetchJobsOverview(signal?: AbortSignal): Promise<AdminResult<JobsOverview>> {
  return adminCall(() => adminClient.get<unknown>('/jobs/overview', { signal }), pickOverview);
}

// GET /api/admin/jobs with the query's filters and page.
export function fetchJobs(query: JobQuery = {}, signal?: AbortSignal): Promise<AdminResult<JobPage>> {
  const params: Record<string, string> = {};
  for (const [key, value] of Object.entries(query)) {
    if (value !== undefined && value !== '') params[key] = String(value);
  }
  return adminCall(() => adminClient.get<unknown>('/jobs', { params, signal }), pickPage);
}

// GET /api/admin/jobs/<id>: one job with its payload.
export function fetchJob(id: number, signal?: AbortSignal): Promise<AdminResult<Job>> {
  return adminCall(() => adminClient.get<unknown>(`/jobs/${id}`, { signal }), pickJobReply);
}

// POST /api/admin/jobs/<id>/retry: a failed job queued again to run now, with
// one attempt more than it has used.
export function retryJob(id: number): Promise<AdminResult<Job>> {
  return adminCall(() => adminClient.post<unknown>(`/jobs/${id}/retry`), pickJobReply);
}

// POST /api/admin/jobs/<id>/cancel: a queued job failed for good.
export function cancelJob(id: number): Promise<AdminResult<Job>> {
  return adminCall(() => adminClient.post<unknown>(`/jobs/${id}/cancel`), pickJobReply);
}

// POST /api/admin/jobs/run: a periodic kind enqueued now.
export function runJobNow(kind: string): Promise<AdminResult<Job>> {
  return adminCall(() => adminClient.post<unknown>('/jobs/run', { kind }), pickJobReply);
}

// POST /api/admin/jobs/prune: the jobs finished more than `keepDays` ago
// deleted by a jobs.prune job.
export function pruneJobs(keepDays: number): Promise<AdminResult<Job>> {
  return adminCall(() => adminClient.post<unknown>('/jobs/prune', { keep_days: keepDays }), pickJobReply);
}
