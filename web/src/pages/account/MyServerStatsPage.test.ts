import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import MyServerStatsPage from './MyServerStatsPage.vue';
import StatsDashboard from '../../components/StatsDashboard.vue';
import { fetchMyServers } from '../../api/hostedServer';
import { fetchAccountStats } from '../../api/hostedStats';
import { hostedServer, serversResponse } from '../../test/hostedFixtures';
import { serverStats } from '../../test/statsFixtures';
import { testRouter } from '../../test/router';
import type { HostedServersResponse } from '../../types/hostedServer';

vi.mock('../../api/hostedServer', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/hostedServer')>()),
  fetchMyServers: vi.fn(),
}));
vi.mock('../../api/hostedStats', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/hostedStats')>()),
  fetchAccountStats: vi.fn(),
}));

let page: ReturnType<typeof mount> | null = null;

async function render(data: HostedServersResponse) {
  vi.mocked(fetchMyServers).mockResolvedValue({ ok: true, value: data });
  const router = await testRouter('/me/servers/stats');
  page = mount(MyServerStatsPage, { global: { plugins: [router] } });
  await flushPromises();
  return page;
}

describe('MyServerStatsPage', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    vi.mocked(fetchAccountStats).mockResolvedValue({ ok: true, value: serverStats({ server_id: undefined, live: null }) });
  });

  afterEach(() => {
    page?.unmount();
    page = null;
  });

  it("shows the account's stats under the Servers tabs, following them while a server is live", async () => {
    await render(serversResponse([hostedServer(1), hostedServer(2, { state: 'stopped' })]));
    expect(page!.get('h1').text()).toBe('Servers');
    expect(page!.findAll('.tab-nav a').map((a) => a.text())).toEqual(['Servers', 'Stats']);
    const dashboard = page!.getComponent(StatsDashboard);
    expect(dashboard.props('variant')).toBe('aggregate');
    expect(dashboard.props('follow')).toBe(true);
    expect(dashboard.props('description')).toBe('');
    expect(fetchAccountStats).toHaveBeenCalled();
  });

  it('stops following once nothing is live', async () => {
    await render(serversResponse([hostedServer(2, { state: 'stopped' })]));
    expect(page!.getComponent(StatsDashboard).props('follow')).toBe(false);
  });

  it('says so in one line when the account has no server', async () => {
    await render(serversResponse([]));
    expect(page!.get('.servers-stats-empty').text()).toBe('No servers yet.');
    expect(page!.findComponent(StatsDashboard).exists()).toBe(false);
  });
});
