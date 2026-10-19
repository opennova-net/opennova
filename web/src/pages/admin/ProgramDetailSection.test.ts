import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import ProgramDetailSection from './ProgramDetailSection.vue';
import { fetchLogs } from '../../api/logs';
import { fetchPrograms, fetchUsage } from '../../api/usage';
import { NOW, logPage, program, programTask, programUsage, programsResponse } from '../../test/usageFixtures';
import { testRouter } from '../../test/router';

vi.mock('../../api/usage', () => ({ fetchPrograms: vi.fn(), fetchUsage: vi.fn() }));
vi.mock('../../api/logs', () => ({ fetchLogs: vi.fn() }));

const programs = vi.mocked(fetchPrograms);
const usage = vi.mocked(fetchUsage);
const logs = vi.mocked(fetchLogs);

describe('ProgramDetailSection', () => {
  let wrapper: ReturnType<typeof mount> | null = null;

  async function mountPage(path = '/admin/infrastructure/programs/web') {
    const router = await testRouter(path, ['/admin/infrastructure/programs/:name']);
    wrapper = mount(ProgramDetailSection, { global: { plugins: [router] } });
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
      tasks: [programTask('aaaa1111bbbb2222'), programTask('cccc3333dddd4444', { reporting: false, sampled_at: null, cpu_percent: null, last_status: 'PENDING' })],
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
    expect(page.text()).toContain('2 of 2 tasks running');
    expect(page.get('.program-now').text()).toBe('web: 41 % CPU of 0.5 vCPU, 612 MiB of 1 GiB, 300 kB/s in, 1.2 MB/s out.');
    const tasks = page.findAll('li.program-task');
    expect(tasks).toHaveLength(2);
    expect(tasks[0].text()).toContain('Reporting');
    expect(tasks[0].text()).toContain('Task aaaa1111bbbb: 41 % CPU of 0.5 vCPU');
    expect(tasks[1].text()).toContain('Not reporting');
    expect(tasks[1].text()).toContain('Pending');
    expect(usage).toHaveBeenCalledWith('program', 'web', '48h', { task: undefined, signal: expect.anything() });
    expect(page.text()).toContain('Usage, all its tasks together');
  });

  it("reads one task's usage, and the log of its tasks on the Logs tab", async () => {
    const page = await mountPage();
    await page.get('#program-task').setValue('aaaa1111bbbb2222');
    await flushPromises();
    expect(usage).toHaveBeenLastCalledWith('program', 'web', '48h', { task: 'aaaa1111bbbb2222', signal: expect.anything() });
    page.unmount();

    const logsPage = await mountPage('/admin/infrastructure/programs/web?tab=logs');
    expect(logs).toHaveBeenCalledWith({ subject: 'program', id: 'web', limit: 200 }, expect.anything());
    expect(logsPage.get('.log-source').text()).toBe('CloudWatch /opennova/prod/service, every stream under web/web/');
    expect(logsPage.get('nav[aria-label="The program\'s views"] [aria-current="page"]').text()).toBe('Logs');
  });

  it('reads the task a link names (?task=, a lobby from the NovaWorld page)', async () => {
    const page = await mountPage('/admin/infrastructure/programs/web?task=cccc3333dddd4444');
    expect((page.get('#program-task').element as HTMLSelectElement).value).toBe('cccc3333dddd4444');
    expect(usage).toHaveBeenCalledWith('program', 'web', '48h', { task: 'cccc3333dddd4444', signal: expect.anything() });
    expect(page.text()).toContain("Task cccc3333dddd's usage");
  });

  it('says the service runs no such program', async () => {
    const page = await mountPage('/admin/infrastructure/programs/hostd');
    expect(page.get('h1').text()).toBe('No such program');
    expect(page.get('[role="alert"]').text()).toBe('The service runs no program called hostd: it runs gate, web.');
  });
});
