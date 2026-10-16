import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import JobsSection from './JobsSection.vue';
import { cancelJob, fetchJobs, fetchJobsOverview, pruneJobs, retryJob, runJobNow } from '../../api/jobs';
import { NOW, ago, job, jobPage, overview } from '../../test/jobsFixtures';
import { testRouter } from '../../test/router';

vi.mock('../../api/jobs', () => ({
  fetchJobsOverview: vi.fn(),
  fetchJobs: vi.fn(),
  retryJob: vi.fn(),
  cancelJob: vi.fn(),
  runJobNow: vi.fn(),
  pruneJobs: vi.fn(),
}));

const readOverview = vi.mocked(fetchJobsOverview);
const readJobs = vi.mocked(fetchJobs);
const retry = vi.mocked(retryJob);
const cancel = vi.mocked(cancelJob);
const runNow = vi.mocked(runJobNow);
const prune = vi.mocked(pruneJobs);

describe('JobsSection', () => {
  let wrapper: ReturnType<typeof mount> | null = null;
  let confirm: MockInstance<typeof window.confirm>;

  async function mountSection() {
    const router = await testRouter('/admin/jobs');
    wrapper = mount(JobsSection, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    vi.useFakeTimers({ toFake: ['Date'] });
    vi.setSystemTime(NOW);
    confirm = vi.spyOn(window, 'confirm').mockReturnValue(true);
    readOverview.mockResolvedValue({ ok: true, value: overview() });
    readJobs.mockResolvedValue({ ok: true, value: jobPage() });
    retry.mockResolvedValue({ ok: true, value: job(77, { state: 'queued' }) });
    cancel.mockResolvedValue({ ok: true, value: job(90, { state: 'failed' }) });
    runNow.mockResolvedValue({ ok: true, value: job(95, { kind: 'hosts.prune', state: 'queued', manual: true }) });
    prune.mockResolvedValue({ ok: true, value: job(96, { kind: 'jobs.prune', state: 'queued' }) });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    confirm.mockRestore();
    vi.useRealTimers();
  });

  it('sums the queue up in words: no raw milliseconds or seconds codes', async () => {
    const page = await mountSection();
    expect(page.get('h1').text()).toBe('Background jobs');
    const tiles = Object.fromEntries(
      page.findAll('dl > div').map((tile) => [tile.get('dt').text(), tile.findAll('dd').map((d) => d.text())]),
    );
    expect(tiles['Waiting now'][0]).toBe('2');
    expect(tiles['Waiting now'][1]).toContain('1 more to run later');
    expect(tiles['Queue latency'][0]).toBe('12 seconds');
    expect(tiles['Running'][0]).toBe('2');
    expect(tiles['Running'][1]).toBe('3 worker threads seen in the last hour');
    expect(tiles['Finished in the last hour'][0]).toBe('35');
    expect(tiles['Finished in the last hour'][1]).toBe('0.6 a minute on average; 800 done in the last day');
    expect(tiles['Failed in the last day'][0]).toBe('2');
    expect(tiles['Mean run time'][0]).toBe('2 seconds');
    expect(page.get('svg[role="img"]').attributes('aria-label')).toBe('Jobs finished a minute, last hour: 5 now, peak 5');
    const text = page.text();
    expect(text).not.toMatch(/\d+ ?ms\b/);
    expect(text).not.toMatch(/\d+s\b/);
    expect(text).not.toContain('_seconds');
  });

  it('warns of a lease run out, a long wait and a scheduler gone quiet', async () => {
    let page = await mountSection();
    let warnings = page.findAll('.jobs-warnings li').map((w) => w.text());
    expect(warnings).toHaveLength(1);
    expect(warnings[0]).toContain("1 running job's lease ran out: its worker went away.");
    page.unmount();
    readOverview.mockResolvedValue({
      ok: true,
      value: overview({
        running: [],
        totals: { ...overview().totals, oldest_wait_seconds: 300 },
        controller: { leader: 'controller-0f0f0f0f', elected_at: ago(7200), last_scheduled_at: ago(600) },
      }),
    });
    page = await mountSection();
    warnings = page.findAll('.jobs-warnings li').map((w) => w.text());
    expect(warnings).toEqual([
      'The oldest due job has waited 5 minutes for a worker: the workers are busy, or none runs.',
      'No schedule has enqueued a job for 10 minutes. Is a controller leading?',
    ]);
  });

  it('lists the failed jobs to look at, a retry for each that can run again', async () => {
    const page = await mountSection();
    const rows = page.findAll('tr.jobs-attention-row');
    expect(rows).toHaveLength(2);
    expect(rows[0].text()).toContain('Launch a fleet machine');
    expect(rows[0].text()).toContain('machine.launch, job 77');
    expect(rows[0].text()).toContain('5 of 5');
    expect(rows[0].text()).toContain('the launch failed');
    expect(rows[0].text()).toContain('10 min ago');
    expect(rows[0].get('a').attributes('href')).toBe('/admin/jobs/77');
    expect(rows[1].text()).toContain('Cannot run again');
    expect(rows[1].find('button').exists()).toBe(false);

    confirm.mockReturnValueOnce(false);
    await rows[0].get('button').trigger('click');
    expect(retry).not.toHaveBeenCalled();
    await rows[0].get('button').trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith(expect.stringContaining('one more attempt, 6 of 6'));
    expect(retry).toHaveBeenCalledWith(77);
    expect(page.text()).toContain('Job 77 is queued again to run now.');
    expect(readOverview).toHaveBeenCalledTimes(2);
  });

  it('shows each kind, the running jobs with their leases, and the workers by process', async () => {
    const page = await mountSection();
    const kinds = page.findAll('tr.jobs-kind-row');
    expect(kinds).toHaveLength(2);
    expect(kinds[1].text()).toContain('2 due');
    expect(kinds[1].text()).toContain('1 later');
    expect(kinds[1].text()).toContain('4 in the last hour');
    expect(kinds[1].text()).toContain('42 seconds');
    expect(kinds[0].text()).toContain('less than a second');
    expect(kinds[0].text()).toContain('None waiting');

    const running = page.findAll('tr.jobs-running-row');
    expect(running).toHaveLength(2);
    expect(running[0].text()).toContain('worker-1a2b3c4d-1');
    expect(running[0].text()).toContain('2 minutes');
    const lapsed = running[0].findAll('td')[2];
    expect(lapsed.text()).toBe('ran out 30 seconds ago');
    expect(lapsed.classes()).toContain('text-danger');
    expect(running[1].findAll('td')[2].text()).toBe('runs out in 57 seconds');

    const workers = page.findAll('tr.jobs-worker-row');
    expect(workers.map((w) => w.get('th').text())).toEqual(['worker-1a2b3c4d-0', 'worker-1a2b3c4d-1', 'worker-5e6f7a8b-0']);
    expect(workers[0].text()).toContain('Idle');
    expect(workers[1].text()).toContain('Launch a fleet machine, job 80');
    expect(workers[1].text()).toContain('2 done, 1 failed');
    expect(workers[2].findAll('td')[0].text()).toBe('worker-5e6f7a8b');
  });

  it('shows the schedules and runs one now, once asked', async () => {
    const page = await mountSection();
    expect(page.get('.jobs-controller').text()).toContain('controller-0f0f0f0f');
    expect(page.get('.jobs-controller').text()).toContain('leading since 2 hours ago');
    const rows = page.findAll('tr.jobs-schedule-row');
    expect(rows).toHaveLength(3);
    expect(rows[0].text()).toContain('Every 30 seconds');
    expect(rows[0].text()).toContain('Done');
    expect(rows[0].text()).toContain('in a moment');
    expect(rows[1].text()).toContain('Every minute');
    expect(rows[1].text()).toContain('only while there is work for it');
    expect(rows[1].text()).toContain('Never');
    expect(rows[1].text()).toContain('When there is work for it');
    expect(rows[2].text()).toContain('Every hour');
    expect(rows[2].text()).toContain('After the one waiting or running');
    expect(rows[2].get('button').attributes('disabled')).toBeDefined();

    await rows[0].get('button').trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith(expect.stringContaining('Run "Prune stale listings and lobby instances" now?'));
    expect(runNow).toHaveBeenCalledWith('hosts.prune');
    expect(page.text()).toContain('is queued to run now (job 95)');

    runNow.mockResolvedValueOnce({
      ok: false,
      error: { code: 'already_pending', status: 409, message: 'A job of that kind, or with the same key, is already waiting or running.' },
    });
    await rows[0].get('button').trigger('click');
    await flushPromises();
    expect(page.text()).toContain('A job of that kind, or with the same key, is already waiting or running.');
  });

  it('filters the list and pages through it, newest first', async () => {
    const page = await mountSection();
    expect(readJobs).toHaveBeenLastCalledWith({ limit: 50 }, expect.anything());
    const rows = page.findAll('tr.jobs-list-row');
    expect(rows).toHaveLength(3);
    expect(rows[0].text()).toContain('Waiting');
    expect(rows[1].text()).toContain('Failed');
    expect(rows[1].text()).toContain('the launch failed');
    expect(rows[2].text()).toContain('hosts.prune@59000');
    expect(page.get('.jobs-page-summary').text()).toBe('Jobs 1 to 3 of 120, newest first');

    const form = page.get('form.jobs-filter');
    await form.get('select[name="state"]').setValue('failed');
    await form.get('select[name="kind"]').setValue('machine.launch');
    await form.get('input[name="from"]').setValue('2026-10-16T09:30');
    await form.get('input[name="q"]').setValue('  launch:7 ');
    await form.trigger('submit');
    await flushPromises();
    expect(readJobs).toHaveBeenLastCalledWith(
      { state: 'failed', kind: 'machine.launch', from: Date.UTC(2026, 9, 16, 9, 30) / 1000, q: 'launch:7', limit: 50 },
      expect.anything(),
    );

    // A range that ends before it starts is refused here.
    await form.get('input[name="to"]').setValue('2026-10-16T09:00');
    await form.trigger('submit');
    expect(page.text()).toContain('The end of the range comes before its start.');
    await form.get('input[name="to"]').setValue('');

    const [newer, older] = page.findAll('nav[aria-label="Job pages"] button');
    expect(newer.attributes('disabled')).toBeDefined();
    await older.trigger('click');
    await flushPromises();
    expect(readJobs).toHaveBeenLastCalledWith(expect.objectContaining({ before: 88, state: 'failed' }), expect.anything());
    expect(page.get('.jobs-page-summary').text()).toBe('Jobs 51 to 53 of 120, newest first');
    await page.findAll('nav[aria-label="Job pages"] button')[0].trigger('click');
    await flushPromises();
    expect(readJobs.mock.lastCall?.[0]).not.toHaveProperty('before');

    await form.findAll('button').find((b) => b.text() === 'Reset')!.trigger('click');
    await flushPromises();
    expect(readJobs).toHaveBeenLastCalledWith({ limit: 50 }, expect.anything());
  });

  it('cancels a waiting job once asked, and shows a refusal', async () => {
    const page = await mountSection();
    const waiting = page.findAll('tr.jobs-list-row')[0];
    expect(waiting.text()).toContain('Cancel');
    expect(waiting.text()).not.toContain('Retry now');
    cancel.mockResolvedValueOnce({
      ok: false,
      error: { code: 'not_queued', status: 409, message: 'That job is no longer waiting: a worker took it, or it ended. Refresh the page.' },
    });
    await waiting.findAll('button').find((b) => b.text().startsWith('Cancel'))!.trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith(expect.stringContaining('Cancel "Send one mail" (job 90)?'));
    expect(cancel).toHaveBeenCalledWith(90);
    expect(page.text()).toContain('That job is no longer waiting: a worker took it, or it ended.');
    await page.findAll('tr.jobs-list-row')[0].findAll('button').find((b) => b.text().startsWith('Cancel'))!.trigger('click');
    await flushPromises();
    expect(page.text()).toContain('Job 90 is cancelled.');
  });

  it('prunes the finished jobs past the keep chosen, once asked', async () => {
    const page = await mountSection();
    const form = page.get('section[aria-labelledby="jobs-prune"] form');
    await form.get('select').setValue('7');
    confirm.mockReturnValueOnce(false);
    await form.trigger('submit');
    expect(prune).not.toHaveBeenCalled();
    await form.trigger('submit');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith(expect.stringContaining('finished more than 7 days ago'));
    expect(prune).toHaveBeenCalledWith(7);
    expect(page.text()).toContain('The finished jobs older than 7 days are being pruned (job 96).');
  });
});
