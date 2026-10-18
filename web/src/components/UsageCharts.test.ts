import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import UsageCharts from './UsageCharts.vue';
import { fetchUsage } from '../api/usage';
import { NOW, machineUsage, programUsage, serverUsage } from '../test/usageFixtures';

vi.mock('../api/usage', () => ({ fetchUsage: vi.fn() }));

const read = vi.mocked(fetchUsage);

describe('UsageCharts', () => {
  let wrapper: ReturnType<typeof mount> | null = null;

  async function mountCharts(props: Record<string, unknown>) {
    wrapper = mount(UsageCharts, { props: { idBase: 'u', ...props } });
    await flushPromises();
    return wrapper;
  }

  function titles(page: ReturnType<typeof mount>) {
    return page.findAll('figure figcaption p:first-child').map((p) => p.text());
  }

  beforeEach(() => {
    vi.clearAllMocks();
    vi.useFakeTimers({ toFake: ['Date'] });
    vi.setSystemTime(NOW);
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    vi.useRealTimers();
  });

  it('names its subject and what it uses now in readable units, then its three charts', async () => {
    read.mockResolvedValue({ ok: true, value: programUsage() });
    const page = await mountCharts({ subject: 'program', id: 'web', name: 'web' });
    expect(read).toHaveBeenCalledWith('program', 'web', '48h', { task: undefined, signal: expect.anything() });
    expect(page.get('.usage-now').text()).toBe('Now. web: 41 % CPU of 1 vCPU, 612 MiB of 2 GiB, 300 kB/s in, 1.2 MB/s out.');
    expect(titles(page)).toEqual(['CPU', 'Memory', 'Network']);
    expect(page.text()).toContain('In percent of its 1 vCPU.');
    expect(page.text()).toContain('against its limit of 2 GiB');
    expect(page.text()).not.toContain('null');
    expect(page.text()).not.toContain('undefined');
  });

  it("adds a server's tick health and a machine's disk", async () => {
    read.mockResolvedValue({ ok: true, value: serverUsage() });
    const server = await mountCharts({ subject: 'server', id: 12, name: 'Server 12' });
    expect(server.get('.usage-now').text()).toBe(
      'Now. Server 12: 41 % CPU of 1 vCPU, 312 MiB of 512 MiB, 300 kB/s in, 1.2 MB/s out, ticks 14.2 ms at the 99th percentile, 3 late ticks in the minute.',
    );
    expect(titles(server)).toEqual(['CPU', 'Memory', 'Network', 'Tick time, 99th percentile', 'Late ticks']);
    server.unmount();

    read.mockResolvedValue({ ok: true, value: machineUsage() });
    const machine = await mountCharts({ subject: 'machine', id: 3, name: 'Machine 3' });
    expect(machine.get('.usage-now').text()).toBe(
      'Now. Machine 3: 22 % CPU, 2 GiB of 4 GiB, 300 kB/s in, 1.2 MB/s out, 30 GB of 40 GB disk free.',
    );
    expect(titles(machine)).toEqual(['CPU', 'Memory', 'Network', 'Disk free']);
    expect(machine.text()).toContain("In percent of the machine's CPUs.");
  });

  it('says so, in words and colour, when nothing reported in the last minutes', async () => {
    read.mockResolvedValue({ ok: true, value: programUsage({ now: null }) });
    const page = await mountCharts({ subject: 'program', id: 'web', name: 'web' });
    const now = page.get('.usage-now');
    expect(now.text()).toBe('Now. web: no report in the last 3 minutes.');
    expect(now.classes()).toContain('text-warn');
  });

  it('switches to the hourly points of 400 days', async () => {
    read.mockResolvedValue({ ok: true, value: programUsage() });
    const page = await mountCharts({ subject: 'program', id: 'web', task: 'abc', name: 'web task abc' });
    await page.get('#u-range-400d').setValue(true);
    await flushPromises();
    expect(read).toHaveBeenLastCalledWith('program', 'web', '400d', { task: 'abc', signal: expect.anything() });
    expect(page.text()).toContain('a point an hour over the last 400 days');
  });

  it("shows a failed read's reason", async () => {
    read.mockResolvedValue({ ok: false, error: { code: 'not_found', status: 404, message: 'It no longer exists. Refresh the list.' } });
    const page = await mountCharts({ subject: 'machine', id: 9, name: 'Machine 9' });
    expect(page.get('[role="alert"]').text()).toBe('It no longer exists. Refresh the list.');
    expect(page.find('.usage-now').exists()).toBe(false);
  });
});
