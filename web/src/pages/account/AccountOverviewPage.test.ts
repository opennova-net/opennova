import { flushPromises, mount, type VueWrapper } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import AccountOverviewPage from './AccountOverviewPage.vue';
import { fetchMyServers } from '../../api/hostedServer';
import { fetchMyBilling } from '../../api/billing';
import { hostedServer, serversResponse } from '../../test/hostedFixtures';
import { myBilling, subscribed } from '../../test/billingFixtures';
import { testRouter } from '../../test/router';

vi.mock('../../api/hostedServer', async (original) => ({ ...(await original<typeof import('../../api/hostedServer')>()), fetchMyServers: vi.fn() }));
vi.mock('../../api/billing', async (original) => ({ ...(await original<typeof import('../../api/billing')>()), fetchMyBilling: vi.fn() }));

let page: VueWrapper | undefined;
async function render() {
  const router = await testRouter('/me');
  page = mount(AccountOverviewPage, { global: { plugins: [router] } });
  await flushPromises();
  return page;
}

describe('AccountOverviewPage', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    vi.mocked(fetchMyServers).mockResolvedValue({ ok: true, value: serversResponse([]) });
    vi.mocked(fetchMyBilling).mockResolvedValue({ ok: true, value: myBilling() });
  });
  afterEach(() => page?.unmount());

  it('keeps playing and downloading prominent for an account without hosting', async () => {
    vi.mocked(fetchMyServers).mockResolvedValue({ ok: true, value: serversResponse([], { entitlement: { max_servers: 0, can_start: false } }) });
    const view = await render();
    expect(view.find('a[href="/lobby"]').exists()).toBe(true);
    expect(view.find('a[href="/downloads"]').exists()).toBe(true);
    expect(view.text()).toContain('No active servers');
    expect(view.find('a[href="/me/servers/new"]').exists()).toBe(false);
    expect(view.text()).toContain('Explore hosting');
  });

  it('shows current servers and existing hosting allowance with a start action', async () => {
    vi.mocked(fetchMyServers).mockResolvedValue({ ok: true, value: serversResponse([hostedServer(4), hostedServer(5, { state: 'stopped' })]) });
    const view = await render();
    expect(view.text()).toContain('1 active');
    expect(view.text()).toContain('1 of 3 hosting places in use');
    expect(view.find('a[href="/me/servers/4"]').exists()).toBe(true);
    expect(view.find('a[href="/me/servers/5"]').exists()).toBe(false);
    expect(view.find('a[href="/me/servers/new"]').exists()).toBe(true);
  });

  it('keeps play and billing available when the hosting read fails', async () => {
    vi.mocked(fetchMyServers).mockResolvedValue({ ok: false, error: { code: 'network', status: null, message: 'Could not read your servers.' } });
    vi.mocked(fetchMyBilling).mockResolvedValue({ ok: true, value: subscribed(2) });
    const view = await render();
    expect(view.get('[role="alert"]').text()).toContain('Could not read your servers');
    expect(view.text()).toContain('Tier 2');
    expect(view.find('a[href="/lobby"]').exists()).toBe(true);
  });

  it('links a payment problem to billing without trying to charge the user', async () => {
    const account = subscribed(1);
    account.subscription!.status = 'past_due';
    vi.mocked(fetchMyBilling).mockResolvedValue({ ok: true, value: account });
    const view = await render();
    const notice = view.findAll('a').find((a) => a.text().includes('A payment needs attention'));
    expect(notice?.attributes('href')).toBe('/me/support');
  });
});
