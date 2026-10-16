import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import JobDetailSection from './JobDetailSection.vue';
import { cancelJob, fetchJob, retryJob } from '../../api/jobs';
import { NOW, ago, job } from '../../test/jobsFixtures';
import { testRouter } from '../../test/router';

vi.mock('../../api/jobs', () => ({ fetchJob: vi.fn(), retryJob: vi.fn(), cancelJob: vi.fn() }));

const read = vi.mocked(fetchJob);
const retry = vi.mocked(retryJob);
const cancel = vi.mocked(cancelJob);

describe('JobDetailSection', () => {
  let wrapper: ReturnType<typeof mount> | null = null;
  let confirm: MockInstance<typeof window.confirm>;

  async function mountSection(id = 77) {
    const router = await testRouter(`/admin/jobs/${id}`, ['/admin/jobs/:id']);
    wrapper = mount(JobDetailSection, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  function field(page: ReturnType<typeof mount>, term: string): string {
    const terms = page.findAll('dl.job-fields dt');
    const at = terms.findIndex((t) => t.text() === term);
    return at < 0 ? '' : page.findAll('dl.job-fields dd')[at].text();
  }

  beforeEach(() => {
    vi.clearAllMocks();
    vi.useFakeTimers({ toFake: ['Date'] });
    vi.setSystemTime(NOW);
    confirm = vi.spyOn(window, 'confirm').mockReturnValue(true);
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    confirm.mockRestore();
    vi.useRealTimers();
  });

  it('shows a failed job in words, its payload, and that it carries no secret', async () => {
    read.mockResolvedValue({
      ok: true,
      value: job(77, {
        kind: 'machine.launch',
        state: 'failed',
        attempts: 5,
        max_attempts: 5,
        last_error: 'the launch failed',
        retryable: true,
        payload: { machine_id: 12, kind: 'shared', slots: 4 },
        dedupe_key: 'machine.launch:12',
      }),
    });
    const page = await mountSection();
    expect(read).toHaveBeenCalledWith(77, expect.anything());
    expect(page.get('h1').text()).toBe('Launch a fleet machine');
    expect(page.text()).toContain('Job 77');
    expect(page.text()).toContain('Failed');
    expect(field(page, 'Attempts')).toBe('5 of 5');
    expect(field(page, 'Created')).toContain('2 min ago');
    expect(field(page, 'Created')).toContain('2026-10-16 11:58 UTC');
    expect(field(page, 'Ended')).toContain('2 min ago');
    expect(field(page, 'Worker')).toBe('worker-1a2b3c4d-0');
    expect(field(page, 'Dedupe key')).toBe('machine.launch:12');
    expect(field(page, 'Schedule key')).toBe('None');
    expect(field(page, 'Secret')).toBe('None');
    expect(field(page, 'Last error')).toBe('the launch failed');
    expect(JSON.parse(page.get('pre.job-payload').text())).toEqual({ machine_id: 12, kind: 'shared', slots: 4 });
    expect(page.text()).not.toMatch(/\d+ ?ms\b/);
  });

  it('retries a failed job with one more attempt, once asked', async () => {
    read.mockResolvedValue({ ok: true, value: job(77, { state: 'failed', attempts: 2, max_attempts: 2, retryable: true }) });
    retry.mockResolvedValue({ ok: true, value: job(77, { state: 'queued', attempts: 2, max_attempts: 3, finished_at: null }) });
    const page = await mountSection();
    const button = page.get('section[aria-label="Actions"] button');
    expect(button.text()).toContain('Retry now: attempt 3 of 3');
    confirm.mockReturnValueOnce(false);
    await button.trigger('click');
    expect(retry).not.toHaveBeenCalled();
    read.mockResolvedValue({ ok: true, value: job(77, { state: 'queued', attempts: 2, max_attempts: 3, finished_at: null }) });
    await button.trigger('click');
    await flushPromises();
    expect(retry).toHaveBeenCalledWith(77);
    expect(page.text()).toContain('Job 77 is queued again to run now.');
    expect(page.text()).toContain('Waiting');
    expect(field(page, 'Attempts')).toBe('2 of 3');
  });

  it('says a job whose secret went cannot run again, and offers no retry', async () => {
    read.mockResolvedValue({
      ok: true,
      value: job(78, { kind: 'mail.send', state: 'failed', secret_state: 'dropped', last_error: 'SES refused the address' }),
    });
    const page = await mountSection(78);
    expect(page.find('section[aria-label="Actions"]').exists()).toBe(false);
    expect(page.text()).toContain('cannot run again');
    expect(field(page, 'Secret')).toBe('It carried one, dropped when it ended.');
  });

  it('shows a running job’s worker and lease, and cancels a waiting one', async () => {
    read.mockResolvedValue({
      ok: true,
      value: job(80, {
        state: 'running',
        finished_at: null,
        running_seconds: 125,
        lease_until: ago(30),
        lease_left_seconds: -30,
        secret_state: 'held',
      }),
    });
    let page = await mountSection(80);
    expect(field(page, 'Running for')).toBe('2 minutes');
    expect(field(page, 'Lease')).toBe('Ran out 30 seconds ago: its worker went away, and the keeper queues it again');
    expect(field(page, 'Secret')).toContain('It carries one, never shown');
    expect(page.find('section[aria-label="Actions"]').exists()).toBe(false);
    page.unmount();

    read.mockResolvedValue({ ok: true, value: job(90, { state: 'queued', finished_at: null, cancellable: true }) });
    cancel.mockResolvedValue({ ok: true, value: job(90, { state: 'failed', last_error: "cancelled by an admin (session 'ops')" }) });
    page = await mountSection(90);
    await page.get('section[aria-label="Actions"] button').trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith(expect.stringContaining('(job 90)? It will not run'));
    expect(cancel).toHaveBeenCalledWith(90);
    expect(page.text()).toContain('Job 90 is cancelled.');
  });

  it('says when the job could not be read', async () => {
    read.mockResolvedValue({ ok: false, error: { code: 'not_found', status: 404, message: 'It no longer exists. Refresh the list.' } });
    const page = await mountSection(5);
    expect(page.text()).toContain('The job could not be loaded');
    expect(page.text()).toContain('It no longer exists. Refresh the list.');
  });
});
