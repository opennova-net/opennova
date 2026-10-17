import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import type { Router } from 'vue-router';
import JobsSection from './JobsSection.vue';
import { cancelJob, fetchJobs, fetchJobsOverview, pruneJobs, retryJob, retryJobGroup, runJobNow } from '../../api/jobs';
import { NOW, ago, job, jobPage, overview, worker } from '../../test/jobsFixtures';
import { testRouter } from '../../test/router';

vi.mock('../../api/jobs', () => ({
  fetchJobsOverview: vi.fn(),
  fetchJobs: vi.fn(),
  retryJob: vi.fn(),
  retryJobGroup: vi.fn(),
  cancelJob: vi.fn(),
  runJobNow: vi.fn(),
  pruneJobs: vi.fn(),
}));

const readOverview = vi.mocked(fetchJobsOverview);
const readJobs = vi.mocked(fetchJobs);
const retry = vi.mocked(retryJob);
const retryGroup = vi.mocked(retryJobGroup);
const cancel = vi.mocked(cancelJob);
const runNow = vi.mocked(runJobNow);
const prune = vi.mocked(pruneJobs);

describe('JobsSection', () => {
  let wrapper: ReturnType<typeof mount> | null = null;
  let router: Router;
  let confirm: MockInstance<typeof window.confirm>;

  async function mountSection(path = '/admin/jobs') {
    router = await testRouter(path);
    wrapper = mount(JobsSection, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  function tiles(page: ReturnType<typeof mount>) {
    return Object.fromEntries(page.findAll('dl > div').map((tile) => [tile.get('dt').text(), tile.findAll('dd').map((d) => d.text())]));
  }

  beforeEach(() => {
    vi.clearAllMocks();
    vi.useFakeTimers({ toFake: ['Date'] });
    vi.setSystemTime(NOW);
    confirm = vi.spyOn(window, 'confirm').mockReturnValue(true);
    readOverview.mockResolvedValue({ ok: true, value: overview() });
    readJobs.mockResolvedValue({ ok: true, value: jobPage() });
    retry.mockResolvedValue({ ok: true, value: job(89, { state: 'queued' }) });
    retryGroup.mockResolvedValue({ ok: true, value: { retried: 2, secret_dropped: 0, already_pending: 0, changed: 0 } });
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

  it('sums the queue up in four cards, in words, over three tabs', async () => {
    const page = await mountSection();
    expect(page.get('h1').text()).toBe('Background jobs');
    const t = tiles(page);
    expect(Object.keys(t)).toEqual(['Queued', 'Running', 'Failed, needs attention', 'Oldest queued wait']);
    expect(t['Queued']).toEqual(['3', '2 due now, 1 to run later']);
    expect(t['Running']).toEqual(['2', '2 of 6 worker threads busy']);
    expect(t['Failed, needs attention']).toEqual(['16', 'In the last day, 2 kinds of failure']);
    expect(t['Oldest queued wait'][0]).toBe('12 seconds');
    const tabs = page.findAll('nav[aria-label="Background jobs views"] a');
    expect(tabs.map((a) => a.text())).toEqual(['Overview', 'Jobs', 'Schedules']);
    expect(tabs[0].attributes('aria-current')).toBe('page');
    expect(tabs[1].attributes('href')).toBe('/admin/jobs?tab=jobs');
    // The overview alone: no list read, no schedule rows.
    expect(readJobs).not.toHaveBeenCalled();
    expect(page.findAll('tr.jobs-schedule-row')).toHaveLength(0);
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
        workers: [worker('worker-5e6f7a8b')],
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

  it('groups alike failures, each to its jobs, with a retry of the ones that can run again', async () => {
    const page = await mountSection();
    const rows = page.findAll('tr.jobs-attention-row');
    expect(rows).toHaveLength(2);
    // Fourteen launches, once: none can run again.
    expect(rows[0].text()).toContain('Launch a fleet machine');
    expect(rows[0].text()).toContain('the launch failed');
    expect(rows[0].text()).toContain('14 jobs');
    expect(rows[0].text()).toContain('none can run again');
    expect(rows[0].text()).toContain('1 min ago');
    expect(rows[0].text()).toContain('Cannot run again');
    expect(rows[0].find('button').exists()).toBe(false);
    expect(rows[0].findAll('a').map((a) => a.attributes('href'))).toContain('/admin/jobs/86');
    // Two mails alike: their shared words, and a retry of both.
    expect(rows[1].text()).toContain('SES refused the address <id>');
    expect(rows[1].text()).toContain('2 jobs');
    expect(rows[1].text()).not.toContain('can run again');
    confirm.mockReturnValueOnce(false);
    await rows[1].get('button').trigger('click');
    expect(retryGroup).not.toHaveBeenCalled();
    await rows[1].get('button').trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith(expect.stringContaining('Retry the 2 failed "Send one mail" jobs that failed this way now?'));
    expect(retryGroup).toHaveBeenCalledWith('mail.send', 'SES refused the address <id>');
    expect(page.text()).toContain('2 jobs are queued again to run now.');
    expect(readOverview).toHaveBeenCalledTimes(2);

    // A group's jobs: the Jobs tab, filtered to them.
    await rows[0].findAll('a').find((a) => a.text().startsWith('Show the jobs'))!.trigger('click');
    await flushPromises();
    expect(router.currentRoute.value.query).toEqual({ tab: 'jobs', state: 'failed', kind: 'machine.launch', failure: 'the launch failed' });
    expect(readJobs).toHaveBeenLastCalledWith(
      { state: 'failed', kind: 'machine.launch', failure: 'the launch failed', limit: 50 },
      expect.anything(),
    );
    expect(page.findAll('tr.jobs-attention-row')).toHaveLength(0);
    expect(page.get('.jobs-failure-filter').text()).toContain('the launch failed');
  });

  it('shows each worker process once, with its threads and leases, and each kind compactly', async () => {
    const page = await mountSection();
    const workers = page.findAll('tr.jobs-worker-row');
    expect(workers.map((w) => w.get('th').text())).toEqual(['worker-1a2b3c4d', 'worker-5e6f7a8b']);
    expect(workers[0].text()).toContain('1 of 2 busy');
    const lapsed = workers[0].findAll('td')[1];
    expect(lapsed.text()).toBe('1 lease ran out: the process went away');
    expect(lapsed.classes()).toContain('text-danger');
    expect(workers[0].text()).toContain('24 done, 1 failed');
    expect(workers[1].text()).toContain('1 of 4 busy');
    expect(workers[1].findAll('td')[1].text()).toBe('Current: the soonest runs out in 57 seconds');

    const kinds = page.findAll('tr.jobs-kind-row');
    expect(kinds).toHaveLength(2);
    expect(kinds[1].text()).toContain('2 due, 1 later');
    expect(kinds[1].text()).toContain('42 seconds');
    expect(kinds[0].text()).toContain('less than a second');
    expect(kinds[0].text()).toContain('None waiting');
  });

  it('shows the schedules on their tab and runs one now, once asked', async () => {
    const page = await mountSection();
    await page.findAll('nav[aria-label="Background jobs views"] a')[2].trigger('click');
    await flushPromises();
    expect(router.currentRoute.value.query).toEqual({ tab: 'schedules' });
    expect(page.findAll('tr.jobs-attention-row')).toHaveLength(0);
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
    await page.findAll('tr.jobs-schedule-row')[0].get('button').trigger('click');
    await flushPromises();
    expect(page.text()).toContain('A job of that kind, or with the same key, is already waiting or running.');
  });

  it('filters the list through the query and pages through it, newest first', async () => {
    const page = await mountSection('/admin/jobs?tab=jobs');
    expect(readJobs).toHaveBeenLastCalledWith({ limit: 50 }, expect.anything());
    const rows = page.findAll('tr.jobs-list-row');
    expect(rows).toHaveLength(4);
    expect(rows[0].text()).toContain('Waiting');
    expect(rows[1].text()).toContain('Failed');
    expect(rows[1].text()).toContain('the launch failed');
    expect(rows[2].text()).toContain('hosts.prune@59000');
    const running = rows[3].get('.jobs-timing');
    expect(running.text()).toContain('running for 2 minutes');
    expect(running.text()).toContain('worker-1a2b3c4d-0');
    expect(running.text()).toContain('lease ran out 30 seconds ago');
    expect(page.get('.jobs-page-summary').text()).toBe('Jobs 1 to 4 of 120, newest first');

    const form = page.get('form.jobs-filter');
    await form.get('select[name="state"]').setValue('failed');
    await form.get('select[name="kind"]').setValue('machine.launch');
    await form.get('input[name="from"]').setValue('2026-10-16T09:30');
    await form.get('input[name="q"]').setValue('  launch:7 ');
    await form.trigger('submit');
    await flushPromises();
    const from = Date.UTC(2026, 9, 16, 9, 30) / 1000;
    expect(router.currentRoute.value.query).toEqual({ tab: 'jobs', state: 'failed', kind: 'machine.launch', from: String(from), q: 'launch:7' });
    expect(readJobs).toHaveBeenLastCalledWith({ state: 'failed', kind: 'machine.launch', from, q: 'launch:7', limit: 50 }, expect.anything());

    // A range that ends before it starts is refused here.
    await form.get('input[name="to"]').setValue('2026-10-16T09:00');
    await form.trigger('submit');
    expect(page.text()).toContain('The end of the range comes before its start.');
    await form.get('input[name="to"]').setValue('');

    const [newer, older] = page.findAll('nav[aria-label="Job pages"] button');
    expect(newer.attributes('disabled')).toBeDefined();
    await older.trigger('click');
    await flushPromises();
    expect(readJobs).toHaveBeenLastCalledWith(expect.objectContaining({ before: 87, state: 'failed' }), expect.anything());
    expect(page.get('.jobs-page-summary').text()).toBe('Jobs 51 to 54 of 120, newest first');
    await page.findAll('nav[aria-label="Job pages"] button')[0].trigger('click');
    await flushPromises();
    expect(readJobs.mock.lastCall?.[0]).not.toHaveProperty('before');

    await form.findAll('button').find((b) => b.text() === 'Reset')!.trigger('click');
    await flushPromises();
    expect(router.currentRoute.value.query).toEqual({ tab: 'jobs' });
    expect(readJobs).toHaveBeenLastCalledWith({ limit: 50 }, expect.anything());
  });

  it('takes a failure group from the query, and lets it go', async () => {
    const page = await mountSection('/admin/jobs?tab=jobs&state=failed&kind=machine.launch&failure=the%20launch%20failed');
    expect(readJobs).toHaveBeenLastCalledWith(
      { state: 'failed', kind: 'machine.launch', failure: 'the launch failed', limit: 50 },
      expect.anything(),
    );
    const form = page.get('form.jobs-filter');
    expect((form.get('select[name="state"]').element as HTMLSelectElement).value).toBe('failed');
    expect((form.get('select[name="kind"]').element as HTMLSelectElement).value).toBe('machine.launch');
    const chip = page.get('.jobs-failure-filter');
    expect(chip.text()).toContain('Only the jobs that failed with');
    await chip.get('a').trigger('click');
    await flushPromises();
    expect(readJobs).toHaveBeenLastCalledWith({ state: 'failed', kind: 'machine.launch', limit: 50 }, expect.anything());
    expect(page.find('.jobs-failure-filter').exists()).toBe(false);
  });

  it('retries a failed job and cancels a waiting one once asked, and shows a refusal', async () => {
    const page = await mountSection('/admin/jobs?tab=jobs');
    const failed = page.findAll('tr.jobs-list-row')[1];
    await failed.findAll('button').find((b) => b.text().startsWith('Retry now'))!.trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith(expect.stringContaining('one more attempt, 2 of 2'));
    expect(retry).toHaveBeenCalledWith(89);
    expect(page.text()).toContain('Job 89 is queued again to run now.');
    expect(readOverview).toHaveBeenCalledTimes(2);

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
    const page = await mountSection('/admin/jobs?tab=jobs');
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
