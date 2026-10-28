import { flushPromises, mount, type VueWrapper } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import AccountOverviewPage from './AccountOverviewPage.vue';
import { fetchMyServers } from '../../api/hostedServer';
import { fetchMyBilling } from '../../api/billing';
import { fetchMyProjects } from '../../api/products';
import { hostedServer, serversResponse } from '../../test/hostedFixtures';
import { myBilling, subscribed } from '../../test/billingFixtures';
import { myProjects } from '../../test/productFixtures';
import { testRouter } from '../../test/router';

vi.mock('../../api/hostedServer', async (original) => ({ ...(await original<typeof import('../../api/hostedServer')>()), fetchMyServers: vi.fn() }));
vi.mock('../../api/billing', async (original) => ({ ...(await original<typeof import('../../api/billing')>()), fetchMyBilling: vi.fn() }));
vi.mock('../../api/products', async (original) => ({ ...(await original<typeof import('../../api/products')>()), fetchMyProjects: vi.fn() }));

let page: VueWrapper | undefined;
async function render() {
  const router = await testRouter('/me');
  page = mount(AccountOverviewPage, { global: { plugins: [router] } });
  await flushPromises();
  return page;
}

function tile(view: VueWrapper, label: string) {
  const found = view.findAll('.stat-tile').find((t) => t.get('dt').text() === label);
  if (!found) throw new Error(`no ${label} tile`);
  return found;
}

describe('AccountOverviewPage', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    vi.mocked(fetchMyServers).mockResolvedValue({ ok: true, value: serversResponse([]) });
    vi.mocked(fetchMyBilling).mockResolvedValue({ ok: true, value: myBilling() });
    vi.mocked(fetchMyProjects).mockResolvedValue({ ok: true, value: myProjects() });
  });
  afterEach(() => page?.unmount());

  it('shows a tile each for servers, projects and billing, each opening its page, and no Play card', async () => {
    const view = await render();
    expect(view.get('h1').text()).toBe('Overview');
    expect(view.findAll('.stat-tile a').map((a) => [a.text(), a.attributes('href')])).toEqual([
      ['Servers', '/me/servers'],
      ['Projects', '/me/projects'],
      ['Billing', '/me/billing'],
    ]);
    expect(view.find('a[href="/lobby"]').exists()).toBe(false);
    expect(view.find('a[href="/downloads"]').exists()).toBe(false);
  });

  it('counts the live servers against the allowance and offers a start', async () => {
    vi.mocked(fetchMyServers).mockResolvedValue({ ok: true, value: serversResponse([hostedServer(4), hostedServer(5, { state: 'stopped' })]) });
    const view = await render();
    expect(tile(view, 'Servers').text()).toContain('1 live');
    expect(tile(view, 'Servers').text()).toContain('1 of 3 in use');
    expect(view.find('a[href="/me/servers/new"]').exists()).toBe(true);
  });

  it('offers no start without an allowance', async () => {
    vi.mocked(fetchMyServers).mockResolvedValue({ ok: true, value: serversResponse([], { entitlement: { max_servers: 0, can_start: false } }) });
    const view = await render();
    expect(tile(view, 'Servers').text()).toContain('No hosting yet');
    expect(view.find('a[href="/me/servers/new"]').exists()).toBe(false);
  });

  it('counts the projects and names the newest', async () => {
    const view = await render();
    expect(tile(view, 'Projects').text()).toContain('2');
    expect(tile(view, 'Projects').text()).toContain('Newest: Rangers');
  });

  it('keeps the other tiles when the hosting read fails', async () => {
    vi.mocked(fetchMyServers).mockResolvedValue({ ok: false, error: { code: 'network', status: null, message: 'Could not read your servers.' } });
    vi.mocked(fetchMyBilling).mockResolvedValue({ ok: true, value: subscribed(2) });
    const view = await render();
    expect(view.get('[role="alert"]').text()).toContain('Could not read your servers');
    expect(tile(view, 'Billing').text()).toContain('Tier 2');
  });

  it('links a payment problem to billing', async () => {
    const account = subscribed(1);
    account.subscription!.status = 'past_due';
    vi.mocked(fetchMyBilling).mockResolvedValue({ ok: true, value: account });
    const view = await render();
    expect(view.get('.past-due a').attributes('href')).toBe('/me/billing');
  });
});
