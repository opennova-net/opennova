import { AxiosError, AxiosHeaders, type AxiosResponse } from 'axios';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { adminClient } from './client';
import { cancelJob, fetchJob, fetchJobs, fetchJobsOverview, pruneJobs, retryJob, runJobNow } from './jobs';

vi.mock('./client', () => ({
  apiClient: { get: vi.fn(), post: vi.fn() },
  adminClient: { get: vi.fn(), post: vi.fn() },
}));

const get = vi.mocked(adminClient.get);
const post = vi.mocked(adminClient.post);

function reply(data: unknown, status = 200) {
  return { status, data } as AxiosResponse;
}

function refusal(status: number, data: unknown): AxiosError {
  const config = { headers: new AxiosHeaders() };
  return new AxiosError('refused', 'ERR_BAD_REQUEST', config, null, {
    status,
    statusText: '',
    data,
    headers: {},
    config,
  } as AxiosResponse);
}

// A job as the service sends it (apps/novaworld_web/jobs/job_routes.cpp job_json).
const wireJob = {
  id: 77,
  kind: 'machine.launch',
  state: 'failed',
  attempts: 5,
  max_attempts: 5,
  last_error: 'the launch failed',
  run_at: '2026-10-16T11:50:00Z',
  created_at: '2026-10-16T11:50:00Z',
  started_at: '2026-10-16T11:55:00Z',
  finished_at: '2026-10-16T11:55:04Z',
  lease_until: null,
  lease_left_seconds: null,
  running_seconds: null,
  worker_id: 'worker-1a2b3c4d-0',
  dedupe_key: 'machine.launch:12',
  schedule_key: null,
  manual: false,
  secret_state: 'dropped',
  retryable: false,
  cancellable: false,
};

const wireKind = {
  queued: 1,
  due: 1,
  running: 0,
  done_hour: 3,
  done_day: 9,
  failed_hour: 0,
  failed_day: 1,
  failed: 1,
  oldest_wait_seconds: 4,
  mean_run_seconds: 0.25,
};

const wireOverview = {
  at: '2026-10-16T12:00:00Z',
  totals: wireKind,
  kinds: [{ kind: 'machine.launch', ...wireKind }],
  per_minute: [{ at: '2026-10-16T11:59:00Z', done: 3, failed: 0 }],
  attention: [wireJob],
  running: [],
  workers: [
    {
      worker_id: 'worker-1a2b3c4d-0',
      process: 'worker-1a2b3c4d',
      job_id: null,
      job_kind: null,
      done: 3,
      failed: 1,
      last_active_at: '2026-10-16T11:55:04Z',
    },
  ],
  schedules: [
    {
      kind: 'hosts.prune',
      interval_seconds: 30,
      at_start: false,
      conditional: false,
      last: null,
      last_finished: null,
      next_at: '2026-10-16T12:00:30Z',
      pending: false,
    },
  ],
  controller: { leader: 'controller-0f0f0f0f', elected_at: '2026-10-16T10:00:00Z', last_scheduled_at: null },
};

describe('the background jobs client', () => {
  beforeEach(() => {
    vi.clearAllMocks();
  });

  it('reads the overview, its secret only as a state', async () => {
    get.mockResolvedValueOnce(reply(wireOverview));
    const result = await fetchJobsOverview();
    expect(get).toHaveBeenCalledWith('/jobs/overview', { signal: undefined });
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    expect(result.value.totals.oldest_wait_seconds).toBe(4);
    expect(result.value.kinds[0].kind).toBe('machine.launch');
    expect(result.value.attention[0]).toMatchObject({ id: 77, secret_state: 'dropped', retryable: false });
    expect(result.value.workers[0].job_id).toBeNull();
    expect(result.value.controller).toEqual({ leader: 'controller-0f0f0f0f', elected_at: '2026-10-16T10:00:00Z', last_scheduled_at: null });
  });

  it('refuses a reply it cannot read', async () => {
    get.mockResolvedValueOnce(reply({ ...wireOverview, attention: [{ id: 'x' }] }));
    const result = await fetchJobsOverview();
    expect(result.ok).toBe(false);
    if (result.ok) return;
    expect(result.error.code).toBe('unknown');
  });

  it('sends the list query as given, leaving out what is unset', async () => {
    get.mockResolvedValueOnce(reply({ jobs: [wireJob], total: 9, next_before: 77 }));
    const result = await fetchJobs({ state: 'failed', kind: 'machine.launch', from: 1792000000, q: '', before: 90, limit: 50 });
    expect(get).toHaveBeenCalledWith('/jobs', {
      params: { state: 'failed', kind: 'machine.launch', from: '1792000000', before: '90', limit: '50' },
      signal: undefined,
    });
    expect(result.ok && result.value.total === 9 && result.value.next_before === 77).toBe(true);
  });

  it('reads one job with its payload', async () => {
    get.mockResolvedValueOnce(reply({ job: { ...wireJob, payload: { machine_id: 12 } } }));
    const result = await fetchJob(77);
    expect(get).toHaveBeenCalledWith('/jobs/77', { signal: undefined });
    expect(result.ok && result.value.payload).toEqual({ machine_id: 12 });
  });

  it('posts each action and maps its refusals to words', async () => {
    post.mockResolvedValue(reply({ job: { ...wireJob, state: 'queued' } }));
    expect((await retryJob(77)).ok).toBe(true);
    expect(post).toHaveBeenLastCalledWith('/jobs/77/retry');
    expect((await cancelJob(78)).ok).toBe(true);
    expect(post).toHaveBeenLastCalledWith('/jobs/78/cancel');
    expect((await runJobNow('hosts.prune')).ok).toBe(true);
    expect(post).toHaveBeenLastCalledWith('/jobs/run', { kind: 'hosts.prune' });
    expect((await pruneJobs(7)).ok).toBe(true);
    expect(post).toHaveBeenLastCalledWith('/jobs/prune', { keep_days: 7 });

    for (const [code, words] of [
      ['secret_dropped', 'cannot run again'],
      ['not_failed', 'has not failed'],
      ['not_queued', 'no longer waiting'],
      ['already_pending', 'already waiting or running'],
    ]) {
      post.mockRejectedValueOnce(refusal(409, { error: code, message: 'server text' }));
      const result = await retryJob(77);
      expect(result.ok).toBe(false);
      if (result.ok) continue;
      expect(result.error.code).toBe(code);
      expect(result.error.message).toContain(words);
      expect(result.error.message).not.toContain('server text');
    }
  });
});
