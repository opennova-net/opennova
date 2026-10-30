import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import ProgramPage from './ProgramPage.vue';
import { fetchLogs } from '../../../api/logs';
import { fetchPrograms, fetchUsage } from '../../../api/usage';
import { NOW, logPage, program, programTask, programUsage, programsResponse } from '../../../test/usageFixtures';
import { testRouter } from '../../../test/router';

vi.mock('../../../api/usage', () => ({ fetchPrograms: vi.fn(), fetchUsage: vi.fn() }));
vi.mock('../../../api/logs', () => ({ fetchLogs: vi.fn() }));

const programs = vi.mocked(fetchPrograms);
const usage = vi.mocked(fetchUsage);
const logs = vi.mocked(fetchLogs);

describe('ProgramPage', () => {
  let wrapper: ReturnType<typeof mount> | null = null;

  async function mountPage(path = '/admin/programs/web') {
    const router = await testRouter(path, ['/admin/programs/:name']);
    wrapper = mount(ProgramPage, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    vi.useFakeTimers({ toFake: ['Date'] });
    vi.setSystemTime(NOW);
    const web = program('web', {
      running: 2,
      desired: 2,
      tasks: [programTask('aaaa1111bbbb2222'), programTask('cccc3333dddd4444', {
          reporting: false,
          sampled_at: null,
          cpu_percent: null,
          last_status: 'PENDING',
          one_version: null,
          one_commit: null,
        })],
    });
    programs.mockResolvedValue({ ok: true, value: programsResponse({ programs: [program('gate'), web] }) });
    usage.mockResolvedValue({ ok: true, value: programUsage() });
    logs.mockResolvedValue({ ok: true, value: logPage({ subject: 'program', id: 'web', generation: null, group: '/opennova/prod/service', stream: 'web/web/' }) });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    vi.useRealTimers();
  });

  it('shows the program, what it uses now, each task, and its usage of all its tasks together', async () => {
    const page = await mountPage();
    expect(page.get('h1').text()).toBe('web');
    expect(page.findAll('nav[aria-label="Breadcrumb"] a').map((a) => [a.text(), a.attributes('href')])).toEqual([
      ['System', '/admin/system'],
      ['Infrastructure', '/admin/system/infrastructure'],
    ]);
    expect(page.get('.page-meta').text()).toContain('2 of 2 tasks running');
    // The build its tasks report, and no ECS task revision beside it.
    expect(page.get('.program-build').text()).toBe('ONE 0.0.22.5922 b0c2b38 (1 task)');
    expect(page.get('.program-build-unknown').text()).toBe('Build not known for 1 task');
    expect(page.get('.page-meta').text()).not.toContain('opennova-prod-web:43');
    expect(page.text()).not.toContain('revision');
    expect(page.find('.program-restarts').exists()).toBe(false);
    expect(page.get('.program-now').text()).toBe('web: 41 % CPU of 0.5 vCPU, 612 MiB of 1 GiB, 300 kB/s in, 1.2 MB/s out.');
    const tasks = page.findAll('tr.program-task');
    expect(tasks).toHaveLength(2);
    expect(tasks[0].text()).toContain('Reporting');
    expect(tasks[0].text()).toContain('Task aaaa1111bbbb: 41 % CPU of 0.5 vCPU');
    expect(tasks[0].text()).toContain('ONE 0.0.22.5922 b0c2b38');
    expect(tasks[1].text()).toContain('Not reporting');
    expect(tasks[1].text()).toContain('Pending');
    expect(tasks[1].text()).toContain('Not reported');
    expect(usage).toHaveBeenCalledWith('program', 'web', '48h', { task: undefined, signal: expect.anything() });
    expect(page.text()).toContain('Usage, all tasks');
    expect(page.get(`nav[aria-label="The program's views"] [aria-current="page"]`).text()).toBe('Usage');
  });

  it("reads one task's usage, and the log of its tasks on the Logs tab", async () => {
    const page = await mountPage();
    await page.get('#program-task').setValue('aaaa1111bbbb2222');
    await flushPromises();
    expect(usage).toHaveBeenLastCalledWith('program', 'web', '48h', { task: 'aaaa1111bbbb2222', signal: expect.anything() });
    page.unmount();

    const logsPage = await mountPage('/admin/programs/web?tab=logs');
    expect(logs).toHaveBeenCalledWith({ subject: 'program', id: 'web', limit: 200 }, expect.anything());
    expect(logsPage.get('.log-source').text()).toBe('CloudWatch /opennova/prod/service, every stream under web/web/');
    expect(logsPage.get('nav[aria-label="The program\'s views"] [aria-current="page"]').text()).toBe('Logs');
  });

  it('reads the task a link names (?task=, a lobby from the NovaWorld tab)', async () => {
    const page = await mountPage('/admin/programs/web?task=cccc3333dddd4444');
    expect((page.get('#program-task').element as HTMLSelectElement).value).toBe('cccc3333dddd4444');
    expect(usage).toHaveBeenCalledWith('program', 'web', '48h', { task: 'cccc3333dddd4444', signal: expect.anything() });
    expect(page.text()).toContain("Task cccc3333dddd's usage");
  });

  it('says the service runs no such program', async () => {
    const page = await mountPage('/admin/programs/hostd');
    expect(page.get('h1').text()).toBe('hostd');
    expect(page.get('.notice[role="alert"]').text()).toBe('No program is called hostd.');
  });

  it('marks its tasks gone in 24 hours in the meta line', async () => {
    programs.mockResolvedValue({ ok: true, value: programsResponse({ programs: [program('web', { restarts_24h: 2 })] }) });
    const page = await mountPage();
    expect(page.get('.program-restarts').text()).toBe('2 tasks gone in 24 hours');
  });
});
