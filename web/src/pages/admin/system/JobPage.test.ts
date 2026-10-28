import * as actionReview from '../../../composables/useConfirm';
import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import JobPage from './JobPage.vue';
import { cancelJob, fetchJob, retryJob } from '../../../api/jobs';
import { NOW, ago, job } from '../../../test/jobsFixtures';
import { testRouter } from '../../../test/router';

vi.mock('../../../api/jobs', () => ({ fetchJob: vi.fn(), retryJob: vi.fn(), cancelJob: vi.fn() }));

const read = vi.mocked(fetchJob);
const retry = vi.mocked(retryJob);
const cancel = vi.mocked(cancelJob);

type Page = ReturnType<typeof mount>;

describe('JobPage', () => {
  let wrapper: Page | null = null;
  let confirm: MockInstance<typeof actionReview.confirmAction>;

  async function mountPage(id = 77) {
    const router = await testRouter(`/admin/jobs/${id}`, ['/admin/jobs/:id']);
    wrapper = mount(JobPage, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  function field(page: Page, term: string): string {
    const terms = page.findAll('dl.job-fields dt');
    const at = terms.findIndex((t) => t.text() === term);
    return at < 0 ? '' : page.findAll('dl.job-fields dd')[at].text();
  }

  function action(page: Page, label: string) {
    return page.get('.page-header header').findAll('button').find((b) => b.text().startsWith(label));
  }

  beforeEach(() => {
    vi.clearAllMocks();
    vi.useFakeTimers({ toFake: ['Date'] });
    vi.setSystemTime(NOW);
    confirm = vi.spyOn(actionReview, 'confirmAction').mockResolvedValue(true);
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    confirm.mockRestore();
    vi.useRealTimers();
  });

  it('heads a job with its crumbs, kind and state, then its details and payload', async () => {
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
    const page = await mountPage();
    expect(read).toHaveBeenCalledWith(77, expect.anything());
    expect(page.get('h1').text()).toBe('Launch a fleet machine');
    expect(page.findAll('nav[aria-label="Breadcrumb"] a').map((a) => [a.text(), a.attributes('href')])).toEqual([
      ['System', '/admin/system'],
      ['Jobs', '/admin/system/jobs'],
      ['Launch a fleet machine jobs', '/admin/system/jobs?tab=jobs&kind=machine.launch'],
    ]);
    const meta = page.get('.page-meta').text();
    expect(meta).toContain('Job 77');
    expect(meta).toContain('machine.launch');
    expect(meta).toContain('Failed');
    expect(field(page, 'Attempts')).toBe('5 of 5');
    expect(field(page, 'Created')).toContain('2 min ago');
    expect(field(page, 'Created')).toContain('2026-10-16 11:58 UTC');
    expect(field(page, 'Worker')).toBe('worker-1a2b3c4d-0');
    expect(field(page, 'Dedupe key')).toBe('machine.launch:12');
    expect(field(page, 'Secret')).toBe('None');
    expect(field(page, 'Last error')).toBe('the launch failed');
    expect(JSON.parse(page.get('pre.job-payload').text())).toEqual({ machine_id: 12, kind: 'shared', slots: 4 });
    expect(page.text()).not.toMatch(/\d+ ?ms\b/);
    expect(page.findAll('p').filter((p) => !p.text().startsWith('Updated'))).toHaveLength(0);
  });

  it('links the ids its payload names, above the payload', async () => {
    read.mockResolvedValue({
      ok: true,
      value: job(81, {
        kind: 'machine.launch',
        payload: {
          machine_id: 12,
          hosted_server_id: 31,
          game_name: 'Night Raid',
          user_id: 7,
          username: 'biggy',
          release: { product_id: 4, product_key: 'JO', version: '1.0' },
        },
      }),
    });
    const page = await mountPage(81);
    const links = page.get('.job-payload-links');
    expect(links.element.compareDocumentPosition(page.get('pre.job-payload').element) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
    expect(links.findAll('a').map((a) => [a.text(), a.attributes('href')])).toEqual([
      ['Machine 12', '/admin/machines/12'],
      ['Night Raid', '/admin/servers/31'],
      ['biggy', '/admin/players/7'],
      ['JO', '/admin/products/4'],
    ]);
  });

  it('shows no link line for a payload that names nothing', async () => {
    read.mockResolvedValue({ ok: true, value: job(82, { kind: 'hosts.prune', payload: { keep_days: 14 } }) });
    const page = await mountPage(82);
    expect(page.find('.job-payload-links').exists()).toBe(false);
  });

  it('retries a failed job with one more attempt, once asked', async () => {
    read.mockResolvedValue({ ok: true, value: job(77, { state: 'failed', attempts: 2, max_attempts: 2, retryable: true }) });
    retry.mockResolvedValue({ ok: true, value: job(77, { state: 'queued', attempts: 2, max_attempts: 3, finished_at: null }) });
    const page = await mountPage();
    expect(action(page, 'Cancel')).toBeUndefined();
    confirm.mockResolvedValueOnce(false);
    await action(page, 'Retry now')!.trigger('click');
    await flushPromises();
    expect(retry).not.toHaveBeenCalled();
    read.mockResolvedValue({ ok: true, value: job(77, { state: 'queued', attempts: 2, max_attempts: 3, finished_at: null }) });
    await action(page, 'Retry now')!.trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith('Retry "Ask the cloud about a fleet machine" (job 77) now? It gets attempt 3.');
    expect(retry).toHaveBeenCalledWith(77);
    expect(page.get('[role="status"]').text()).toBe('Job 77 is queued again.');
    expect(page.get('.page-meta').text()).toContain('Waiting');
    expect(field(page, 'Attempts')).toBe('2 of 3');
  });

  it('says a failed job whose secret went cannot run again, and offers no retry', async () => {
    read.mockResolvedValue({ ok: true, value: job(78, { kind: 'mail.send', state: 'failed', secret_state: 'dropped', last_error: 'SES refused' }) });
    const page = await mountPage(78);
    expect(action(page, 'Retry now')).toBeUndefined();
    expect(field(page, 'Secret')).toBe('Dropped: it cannot run again');
  });

  it("shows a running job's lease, and cancels a waiting one once asked", async () => {
    read.mockResolvedValue({
      ok: true,
      value: job(80, { state: 'running', finished_at: null, running_seconds: 125, lease_until: ago(30), lease_left_seconds: -30, secret_state: 'held' }),
    });
    let page = await mountPage(80);
    expect(field(page, 'Running for')).toBe('2 minutes');
    expect(field(page, 'Lease')).toBe('Ran out 30 seconds ago');
    expect(field(page, 'Secret')).toBe('Held, never shown');
    expect(action(page, 'Cancel')).toBeUndefined();
    page.unmount();

    read.mockResolvedValue({ ok: true, value: job(90, { state: 'queued', finished_at: null, cancellable: true }) });
    cancel.mockResolvedValue({ ok: true, value: job(90, { state: 'failed', last_error: "cancelled by an admin (session 'ops')" }) });
    page = await mountPage(90);
    await action(page, 'Cancel')!.trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith('Cancel "Ask the cloud about a fleet machine" (job 90)? It will not run.');
    expect(cancel).toHaveBeenCalledWith(90);
    expect(page.get('[role="status"]').text()).toBe('Job 90 is cancelled.');
  });

  it('says when the job could not be read, and reads again on asking', async () => {
    read.mockResolvedValue({ ok: false, error: { code: 'not_found', status: 404, message: 'It no longer exists.' } });
    const page = await mountPage(5);
    expect(page.get('h1').text()).toBe('Job 5');
    expect(page.findAll('nav[aria-label="Breadcrumb"] a').map((a) => a.text())).toEqual(['System', 'Jobs']);
    expect(page.get('.job-missing').text()).toContain('The job could not be loaded.');
    expect(page.get('[role="alert"]').text()).toBe('It no longer exists.');
    read.mockResolvedValue({ ok: true, value: job(5) });
    await page.get('.job-missing button').trigger('click');
    await flushPromises();
    expect(page.find('.job-missing').exists()).toBe(false);
    expect(page.get('h1').text()).toBe('Ask the cloud about a fleet machine');
  });
});
