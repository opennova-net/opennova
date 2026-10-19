import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import UsageSection from './UsageSection.vue';
import { fetchFleetUsage, parseFleetUsage } from '../../api/hostedStats';
import { fleetUsageReply } from '../../test/statsFixtures';
import { testRouter } from '../../test/router';

vi.mock('../../api/hostedStats', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/hostedStats')>()),
  fetchFleetUsage: vi.fn(),
}));

const usage = vi.mocked(fetchFleetUsage);

describe('UsageSection', () => {
  let wrapper: ReturnType<typeof mount> | null = null;

  async function mountSection() {
    const router = await testRouter('/admin/usage');
    wrapper = mount(UsageSection, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    usage.mockResolvedValue({ ok: true, value: parseFleetUsage(fleetUsageReply())! });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    vi.useRealTimers();
  });

  it("shows the fleet's totals as tiles in readable units", async () => {
    const page = await mountSection();
    const tiles = page.findAll('dl > div').map((t) => t.text());
    expect(tiles).toHaveLength(6);
    expect(tiles[0]).toContain('Live servers');
    expect(tiles[0]).toContain('4');
    expect(tiles[0]).toContain('6 players on them; 1 reporting their memory and CPU');
    expect(tiles[1]).toContain('3 of 13 slots hold a server');
    expect(tiles[2]).toContain('200 MiB');
    expect(tiles[2]).toContain('Of 512 MiB');
    expect(tiles[3]).toContain('0.25 vCPU');
    expect(tiles[3]).toContain('Of 1 vCPU,');
    // 150,000 bytes a second is 1.2 megabits a second.
    expect(tiles[4]).toContain('1.2 Mb/s');
    expect(tiles[5]).toContain('Sent to players in the last hour');
    expect(tiles[5]).toContain('540 MB');
  });

  it("lists each machine's share", async () => {
    const page = await mountSection();
    const rows = page.findAll('tr.usage-machine-row');
    expect(rows).toHaveLength(2);
    expect(rows[0].text()).toContain('Machine 3');
    expect(rows[0].text()).toContain('Shared machine, t3a.large');
    expect(rows[0].text()).toContain('2 of 12');
    expect(rows[0].text()).toContain('200 MiB of 512 MiB');
    expect(rows[0].text()).toContain('0.25 vCPU of 1 vCPU');
    expect(rows[0].text()).toContain('520 MB');
    // Its server has no recent sample: no memory or CPU to show.
    expect(rows[1].text()).toContain('Own machine, c7a.medium');
    expect(rows[1].text()).toContain('No report');
    expect(rows[1].text()).toContain('0 b/s');
  });

  it('lists the servers by machine and slot, each with its numbers now and over the last hour', async () => {
    const page = await mountSection();
    const rows = page.findAll('tr.usage-server-row');
    expect(rows.map((r) => r.get('th a').text())).toEqual(['Booting One', 'Night Ops', 'Own Box', 'Waiting']);
    const running = rows[1].text();
    expect(rows[1].get('th a').attributes('href')).toBe('/admin/fleet/7');
    expect(running).toContain('owner1');
    expect(running).toContain('Machine 3, slot 1');
    expect(running).toContain('Up 3 hours 5 minutes');
    expect(running).toContain('6 of 32');
    expect(running).toContain('200 MiB of 512 MiB');
    expect(running).toContain('Peak 310 MiB in the last hour');
    expect(running).toContain('25 %');
    expect(running).toContain('Mean 18 %, peak 61 %');
    expect(running).toContain('1.2 Mb/s');
    expect(running).toContain('520 MB in the last hour');
    // Booting: a sample without its container's numbers.
    expect(rows[0].text()).toContain('Booting');
    expect(rows[0].text()).toContain('Not listed');
    expect(rows[0].text()).toContain('No report');
    expect(rows[0].text()).toContain('0 b/s');
    // Its account gone, its newest sample too old.
    expect(rows[2].text()).toContain('Deleted account');
    expect(rows[2].text()).toContain('own machine');
    expect(rows[2].text()).toContain('Peak 100 MiB in the last hour');
    expect(rows[3].text()).toContain('Not placed');
  });

  it('reads again every 30 seconds', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout', 'Date'] });
    await mountSection();
    expect(usage).toHaveBeenCalledTimes(1);
    await vi.advanceTimersByTimeAsync(29_000);
    expect(usage).toHaveBeenCalledTimes(1);
    await vi.advanceTimersByTimeAsync(1_000);
    expect(usage).toHaveBeenCalledTimes(2);
  });

  it("says why a read failed, in the page's words", async () => {
    usage.mockResolvedValue({ ok: false, error: { code: 'forbidden', status: 403, message: 'This account is not an admin.' } });
    const page = await mountSection();
    expect(page.get('[role="alert"]').text()).toBe('This account is not an admin.');
    expect(page.text()).toContain('Not loaded.');
  });
});
