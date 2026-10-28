import * as actionReview from '../../../composables/useConfirm';
import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import type { Router } from 'vue-router';
import JobsTab from './JobsTab.vue';
import { cancelJob, dismissJobGroup, fetchJobs, fetchJobsOverview, pruneJobs, retryJob, retryJobGroup, runJobNow } from '../../../api/jobs';
import { NOW, ago, job, jobPage, overview, worker } from '../../../test/jobsFixtures';
import { testRouter } from '../../../test/router';

vi.mock('../../../api/jobs', () => ({
  fetchJobsOverview: vi.fn(),
  fetchJobs: vi.fn(),
  retryJob: vi.fn(),
  retryJobGroup: vi.fn(),
  dismissJobGroup: vi.fn(),
  cancelJob: vi.fn(),
  runJobNow: vi.fn(),
  pruneJobs: vi.fn(),
}));

const readOverview = vi.mocked(fetchJobsOverview);
const readJobs = vi.mocked(fetchJobs);
const dismissGroup = vi.mocked(dismissJobGroup);
const retry = vi.mocked(retryJob);
const retryGroup = vi.mocked(retryJobGroup);
const cancel = vi.mocked(cancelJob);
const runNow = vi.mocked(runJobNow);
const prune = vi.mocked(pruneJobs);

type Page = ReturnType<typeof mount>;

describe('JobsTab', () => {
  let wrapper: Page | null = null;
  let router: Router;
  let confirm: MockInstance<typeof actionReview.confirmAction>;

  async function mountTab(path = '/admin/system/jobs') {
    router = await testRouter(path);
    wrapper = mount(JobsTab, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  function tiles(page: Page) {
    return Object.fromEntries(page.findAll('.jobs-tiles .stat-tile').map((tile) => [tile.get('dt').text(), tile.findAll('dd').map((d) => d.text())]));
  }

  function views(page: Page) {
    return page.findAll('nav[aria-label="Jobs views"] a');
  }

  function button(row: { findAll: Page['findAll'] }, label: string) {
    return row.findAll('button').find((b) => b.text().startsWith(label))!;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    vi.useFakeTimers({ toFake: ['Date'] });
    vi.setSystemTime(NOW);
    confirm = vi.spyOn(actionReview, 'confirmAction').mockResolvedValue(true);
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

  it("is System's Jobs page: its tabs, four tiles and the views, with no prose", async () => {
    const page = await mountTab();
    expect(page.get('h1').text()).toBe('System');
    const systemTabs = page.findAll('nav[aria-label="System pages"] a');
    expect(systemTabs.map((a) => a.text())).toEqual(['NovaWorld', 'Infrastructure', 'Jobs']);
    expect(systemTabs[2].attributes('aria-current')).toBe('page');

    const t = tiles(page);
    expect(Object.keys(t)).toEqual(['Queued', 'Running', 'Failed today', 'Oldest wait']);
    expect(t['Queued']).toEqual(['3', '2 due, 1 later']);
    expect(t['Running']).toEqual(['2', '2 of 6 threads busy']);
    expect(t['Failed today']).toEqual(['16', '2 kinds of failure']);
    expect(t['Oldest wait']).toEqual(['12 seconds']);
    const tileLinks = page.findAll('.jobs-tiles a').map((a) => a.attributes('href'));
    expect(tileLinks).toEqual([
      '/admin/system/jobs?tab=jobs&state=queued',
      '/admin/system/jobs?tab=jobs&state=running',
      '/admin/system/jobs?tab=jobs&state=failed',
    ]);

    expect(views(page).map((a) => [a.text(), a.attributes('href'), a.attributes('aria-current')])).toEqual([
      ['Overview', '/admin/system/jobs', 'page'],
      ['Jobs', '/admin/system/jobs?tab=jobs', undefined],
      ['Schedules', '/admin/system/jobs?tab=schedules', undefined],
    ]);
    // The overview alone: no list read, no schedule rows.
    expect(readJobs).not.toHaveBeenCalled();
    expect(page.findAll('tr.jobs-schedule-row')).toHaveLength(0);
    expect(page.findAll('section p').filter((p) => !p.classes('jobs-attention-more'))).toHaveLength(0);
    const text = page.text();
    expect(text).not.toMatch(/\d+ ?ms\b/);
    expect(text).not.toContain('_seconds');
  });

  it('keeps the System tab current on every view', async () => {
    const page = await mountTab('/admin/system/jobs?tab=schedules');
    expect(page.findAll('nav[aria-label="System pages"] a')[2].attributes('aria-current')).toBe('page');
    expect(views(page)[2].attributes('aria-current')).toBe('page');
    expect(views(page)[0].attributes('aria-current')).toBeUndefined();
  });

  it('warns in one sentence each of a lease run out, a long wait and a quiet scheduler', async () => {
    let page = await mountTab();
    expect(page.findAll('.jobs-warnings li').map((w) => w.text())).toEqual(["1 running job's lease ran out."]);
    page.unmount();
    readOverview.mockResolvedValue({
      ok: true,
      value: overview({
        workers: [worker('worker-5e6f7a8b')],
        totals: { ...overview().totals, oldest_wait_seconds: 300 },
        last_scheduled_at: ago(600),
      }),
    });
    page = await mountTab();
    expect(page.findAll('.jobs-warnings li').map((w) => w.text())).toEqual([
      'The oldest due job has waited 5 minutes.',
      'No schedule has enqueued a job for 10 minutes.',
    ]);
    expect(page.findAll('.jobs-warnings li').every((li) => li.classes().includes('notice-warn'))).toBe(true);
  });

  it('groups alike failures, each to its jobs and its last job, retried or dismissed once asked', async () => {
    const page = await mountTab();
    const rows = page.findAll('tr.jobs-attention-row');
    expect(rows).toHaveLength(2);
    expect(rows[0].text()).toContain('Launch a fleet machine');
    expect(rows[0].text()).toContain('the launch failed');
    expect(rows[0].text()).toContain('14 jobs');
    expect(rows[0].text()).toContain('none can run again');
    expect(rows[0].text()).toContain('1 min ago');
    expect(rows[0].get('a.job-link').attributes('href')).toBe('/admin/jobs/86');
    expect(rows[0].text()).not.toContain('Retry all');

    dismissGroup.mockResolvedValue({ ok: true, value: 14 });
    await button(rows[0], 'Dismiss').trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith('Dismiss these 14 failed jobs? They are deleted.');
    expect(dismissGroup).toHaveBeenCalledWith('machine.launch', 'the launch failed');

    expect(rows[1].text()).toContain('SES refused the address <id>');
    confirm.mockResolvedValueOnce(false);
    await button(rows[1], 'Retry all').trigger('click');
    expect(retryGroup).not.toHaveBeenCalled();
    await button(rows[1], 'Retry all').trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith('Retry 2 failed "Send one mail" jobs now? Each gets one more attempt.');
    expect(retryGroup).toHaveBeenCalledWith('mail.send', 'SES refused the address <id>');
    expect(page.get('[role="status"]').text()).toBe('2 jobs queued again.');
    expect(readOverview).toHaveBeenCalledTimes(3);

    // A group's jobs: the Jobs view, filtered to them.
    const show = rows[0].findAll('a').find((a) => a.text().startsWith('Show'))!;
    expect(show.attributes('href')).toBe('/admin/system/jobs?tab=jobs&state=failed&kind=machine.launch&failure=the+launch+failed');
    await show.trigger('click');
    await flushPromises();
    expect(router.currentRoute.value.query).toEqual({ tab: 'jobs', state: 'failed', kind: 'machine.launch', failure: 'the launch failed' });
    expect(readJobs).toHaveBeenLastCalledWith({ state: 'failed', kind: 'machine.launch', failure: 'the launch failed', limit: 50 }, expect.anything());
    expect(page.get('.jobs-failure-filter').text()).toContain('the launch failed');
  });

  it('shows each worker and each kind compactly, each kind to its jobs', async () => {
    const page = await mountTab();
    const workers = page.findAll('tr.jobs-worker-row');
    expect(workers.map((w) => w.get('th').text())).toEqual(['worker-1a2b3c4d', 'worker-5e6f7a8b']);
    expect(workers[0].get('a').attributes('href')).toBe('/admin/system/jobs?tab=jobs&state=running');
    expect(workers[0].findAll('td')[1].text()).toBe('1 lease ran out');
    expect(workers[0].findAll('td')[1].classes()).toContain('text-danger');
    expect(workers[0].text()).toContain('24 done, 1 failed');
    expect(workers[1].findAll('td')[1].text()).toBe('Soonest runs out in 57 seconds');

    const kinds = page.findAll('tr.jobs-kind-row');
    expect(kinds).toHaveLength(2);
    expect(kinds[1].get('a').attributes('href')).toBe('/admin/system/jobs?tab=jobs&kind=machine.launch');
    expect(kinds[1].text()).toContain('2 due, 1 later');
    expect(kinds[1].text()).toContain('42 seconds');
    expect(kinds[0].text()).toContain('less than a second');
  });

  it('shows the schedules alone (no fleet leader) and runs one now, once asked', async () => {
    const page = await mountTab();
    await views(page)[2].trigger('click');
    await flushPromises();
    expect(router.currentRoute.value.query).toEqual({ tab: 'schedules' });
    expect(page.text()).not.toContain('fleet-0f0f0f0f');
    expect(page.text()).not.toMatch(/leader/i);
    const rows = page.findAll('tr.jobs-schedule-row');
    expect(rows).toHaveLength(3);
    expect(rows[0].text()).toContain('Every 30 seconds');
    expect(rows[0].get('a').attributes('href')).toBe('/admin/jobs/900');
    expect(rows[0].text()).toContain('Done');
    expect(rows[1].text()).toContain('only with work');
    expect(rows[1].text()).toContain('Never');
    expect(rows[2].text()).toContain('After the one waiting or running');
    expect(rows[2].get('button').attributes('disabled')).toBeDefined();

    await rows[0].get('button').trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith('Run "Prune stale listings and lobby instances" now? A job is queued beside its schedule.');
    expect(runNow).toHaveBeenCalledWith('hosts.prune');
    expect(page.get('[role="status"]').text()).toBe('"Prune stale listings and lobby instances" is queued (job 95).');

    runNow.mockResolvedValueOnce({ ok: false, error: { code: 'already_pending', status: 409, message: 'One is already waiting or running.' } });
    await page.findAll('tr.jobs-schedule-row')[0].get('button').trigger('click');
    await flushPromises();
    expect(page.get('[role="alert"]').text()).toBe('One is already waiting or running.');
  });

  it('lists jobs by JobLink, filters them from one row into the query and pages through them', async () => {
    const page = await mountTab('/admin/system/jobs?tab=jobs');
    expect(views(page)[1].attributes('aria-current')).toBe('page');
    expect(readJobs).toHaveBeenLastCalledWith({ limit: 50 }, expect.anything());
    const rows = page.findAll('tr.jobs-list-row');
    expect(rows.map((r) => [r.get('a.job-link').text(), r.get('a.job-link').attributes('href')])).toEqual([
      ['Send one mail', '/admin/jobs/90'],
      ['Launch a fleet machine', '/admin/jobs/89'],
      ['Prune stale listings and lobby instances', '/admin/jobs/88'],
      ['Sync the customers Stripe sent events for', '/admin/jobs/87'],
    ]);
    expect(rows[1].text()).toContain('the launch failed');
    const running = rows[3].get('.jobs-timing');
    expect(running.text()).toContain('running for 2 minutes');
    expect(running.text()).toContain('lease ran out 30 seconds ago');
    expect(page.get('.jobs-page-summary').text()).toBe('1 to 4 of 120');

    const form = page.get('form.jobs-filter');
    expect(form.findAll('select, input').every((f) => f.classes().includes('w-auto'))).toBe(true);
    await form.get('select[name="state"]').setValue('failed');
    await form.get('select[name="kind"]').setValue('machine.launch');
    await form.get('input[name="from"]').setValue('2026-10-16T09:30');
    await form.get('input[name="q"]').setValue('  launch:7 ');
    await form.trigger('submit');
    await flushPromises();
    const from = Date.UTC(2026, 9, 16, 9, 30) / 1000;
    expect(router.currentRoute.value.path).toBe('/admin/system/jobs');
    expect(router.currentRoute.value.query).toEqual({ tab: 'jobs', state: 'failed', kind: 'machine.launch', from: String(from), q: 'launch:7' });
    expect(readJobs).toHaveBeenLastCalledWith({ state: 'failed', kind: 'machine.launch', from, q: 'launch:7', limit: 50 }, expect.anything());

    await form.get('input[name="to"]').setValue('2026-10-16T09:00');
    await form.trigger('submit');
    expect(page.text()).toContain('The end comes before the start.');
    await form.get('input[name="to"]').setValue('');

    const [newer, older] = page.findAll('nav[aria-label="Job pages"] button');
    expect(newer.attributes('disabled')).toBeDefined();
    await older.trigger('click');
    await flushPromises();
    expect(readJobs).toHaveBeenLastCalledWith(expect.objectContaining({ before: 87, state: 'failed' }), expect.anything());
    expect(page.get('.jobs-page-summary').text()).toBe('51 to 54 of 120');
    await page.findAll('nav[aria-label="Job pages"] button')[0].trigger('click');
    await flushPromises();
    expect(readJobs.mock.lastCall?.[0]).not.toHaveProperty('before');

    await button(form, 'Reset').trigger('click');
    await flushPromises();
    expect(router.currentRoute.value.query).toEqual({ tab: 'jobs' });
    expect(readJobs).toHaveBeenLastCalledWith({ limit: 50 }, expect.anything());
  });

  it('takes a failure group from the query, and clears it', async () => {
    const page = await mountTab('/admin/system/jobs?tab=jobs&state=failed&kind=machine.launch&failure=the%20launch%20failed');
    const form = page.get('form.jobs-filter');
    expect((form.get('select[name="state"]').element as HTMLSelectElement).value).toBe('failed');
    expect((form.get('select[name="kind"]').element as HTMLSelectElement).value).toBe('machine.launch');
    const chip = page.get('.jobs-failure-filter');
    expect(chip.get('a').attributes('href')).toBe('/admin/system/jobs?tab=jobs&state=failed&kind=machine.launch');
    await chip.get('a').trigger('click');
    await flushPromises();
    expect(readJobs).toHaveBeenLastCalledWith({ state: 'failed', kind: 'machine.launch', limit: 50 }, expect.anything());
    expect(page.find('.jobs-failure-filter').exists()).toBe(false);
  });

  it('retries a failed job and cancels a waiting one once asked, and shows a refusal', async () => {
    const page = await mountTab('/admin/system/jobs?tab=jobs');
    await button(page.findAll('tr.jobs-list-row')[1], 'Retry now').trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith('Retry "Launch a fleet machine" (job 89) now? It gets attempt 2.');
    expect(retry).toHaveBeenCalledWith(89);
    expect(page.get('[role="status"]').text()).toBe('Job 89 is queued again.');
    expect(readOverview).toHaveBeenCalledTimes(2);

    const waiting = page.findAll('tr.jobs-list-row')[0];
    expect(waiting.text()).not.toContain('Retry now');
    cancel.mockResolvedValueOnce({ ok: false, error: { code: 'not_queued', status: 409, message: 'That job is no longer waiting.' } });
    await button(waiting, 'Cancel').trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith('Cancel "Send one mail" (job 90)? It will not run.');
    expect(page.get('[role="alert"]').text()).toBe('That job is no longer waiting.');
    await button(page.findAll('tr.jobs-list-row')[0], 'Cancel').trigger('click');
    await flushPromises();
    expect(page.get('[role="status"]').text()).toBe('Job 90 is cancelled.');
  });

  it('keeps the prune behind its toggle, and prunes past the keep chosen once asked', async () => {
    const page = await mountTab('/admin/system/jobs?tab=jobs');
    const details = page.get('details.jobs-prune');
    expect((details.element as HTMLDetailsElement).open).toBe(false);
    expect(details.get('summary').text()).toBe('Prune finished jobs');
    const form = details.get('form');
    await form.get('select').setValue('7');
    confirm.mockResolvedValueOnce(false);
    await form.trigger('submit');
    expect(prune).not.toHaveBeenCalled();
    await form.trigger('submit');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith('Delete every job that finished more than 7 days ago? Their history is gone for good.');
    expect(prune).toHaveBeenCalledWith(7);
    expect(page.get('[role="status"]').text()).toBe('Pruning jobs older than 7 days (job 96).');
  });

  it("shows a failed read in the page's alert", async () => {
    readOverview.mockResolvedValue({ ok: false, error: { code: 'network', status: null, message: 'Could not reach the server.' } });
    const page = await mountTab();
    expect(page.get('[role="alert"]').text()).toBe('Could not reach the server.');
    expect(page.find('tr.jobs-attention-row').exists()).toBe(false);
    expect(page.text()).toContain('Not loaded.');
  });
});
