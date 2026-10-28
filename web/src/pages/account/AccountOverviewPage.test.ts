import { flushPromises, mount, type VueWrapper } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { Router } from 'vue-router';
import AccountOverviewPage from './AccountOverviewPage.vue';
import { fetchMyAddresses } from '../../api/addresses';
import { fetchMyBilling } from '../../api/billing';
import { fetchMyServers } from '../../api/hostedServer';
import { listMyServers } from '../../api/myServers';
import { fetchMyProjects, fetchProjectReleases } from '../../api/products';
import { addressesResponse } from '../../test/addressFixtures';
import { addressSubscription, myBilling, subscribed } from '../../test/billingFixtures';
import { hostedServer, listedServer, serversResponse } from '../../test/hostedFixtures';
import { myProjects, nightOpsRelease } from '../../test/productFixtures';
import { testRouter } from '../../test/router';

const { refreshSession } = vi.hoisted(() => ({ refreshSession: vi.fn() }));

vi.mock('../../api/hostedServer', async (original) => ({ ...(await original<typeof import('../../api/hostedServer')>()), fetchMyServers: vi.fn() }));
vi.mock('../../api/myServers', async (original) => ({ ...(await original<typeof import('../../api/myServers')>()), listMyServers: vi.fn() }));
vi.mock('../../api/billing', async (original) => ({ ...(await original<typeof import('../../api/billing')>()), fetchMyBilling: vi.fn() }));
vi.mock('../../api/addresses', async (original) => ({ ...(await original<typeof import('../../api/addresses')>()), fetchMyAddresses: vi.fn() }));
vi.mock('../../api/products', async (original) => ({
  ...(await original<typeof import('../../api/products')>()),
  fetchMyProjects: vi.fn(),
  fetchProjectReleases: vi.fn(),
}));
vi.mock('../../composables/useSession', async () => {
  const { shallowRef } = await import('vue');
  return { useSession: () => ({ user: shallowRef({ username: 'biggy', role: 'player' }), refresh: refreshSession }) };
});

const releases = vi.mocked(fetchProjectReleases);

// An ISO time `ms` before now.
const ago = (ms: number) => new Date(Date.now() - ms).toISOString();
const HOUR = 3_600_000;

let page: VueWrapper | undefined;
let router: Router;
async function render() {
  router = await testRouter('/me');
  page = mount(AccountOverviewPage, { global: { plugins: [router] } });
  await flushPromises();
  return page;
}

function tile(view: VueWrapper, label: string) {
  const found = view.findAll('.stat-tile').find((t) => t.get('dt').text() === label);
  if (!found) throw new Error(`no ${label} tile`);
  return found;
}

// Each Needs-you row: its badge, its subject and where it links, and its text.
function needs(view: VueWrapper) {
  return view.findAll('.needs-you-item').map((li) => ({
    badge: li.get('.badge').text(),
    subject: li.get('a').text(),
    link: li.get('a').attributes('href'),
    text: li.get('.needs-you-text').text(),
  }));
}

describe('AccountOverviewPage', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    vi.mocked(fetchMyServers).mockResolvedValue({ ok: true, value: serversResponse([]) });
    vi.mocked(listMyServers).mockResolvedValue({ ok: true, value: [] });
    vi.mocked(fetchMyBilling).mockResolvedValue({ ok: true, value: myBilling() });
    vi.mocked(fetchMyAddresses).mockResolvedValue({ ok: true, value: addressesResponse([]) });
    vi.mocked(fetchMyProjects).mockResolvedValue({ ok: true, value: myProjects() });
    releases.mockResolvedValue({ ok: true, value: [] });
  });
  afterEach(() => page?.unmount());

  it('heads the page with Start a server and Refresh, then a tile each opening its section, then Needs you', async () => {
    const view = await render();
    expect(view.get('h1').text()).toBe('Overview');
    expect(view.find('.page-header p:not(.text-xs)').exists()).toBe(false);
    expect(view.get('.page-header a.btn-primary').attributes('href')).toBe('/me/servers/new');
    expect(view.findAll('.page-header button').map((b) => b.text())).toEqual(['Refresh']);
    expect(view.findAll('.stat-tile a').map((a) => [a.text(), a.attributes('href')])).toEqual([
      ['Servers', '/me/servers'],
      ['Projects', '/me/projects'],
      ['Billing', '/me/billing'],
    ]);
    expect(view.get('#needs-you-heading').text()).toBe('Needs you');
    expect(view.get('.needs-you-empty').text()).toBe('Nothing needs you.');
  });

  it("counts the live servers, the games hosted from the game among them, and the players on them", async () => {
    vi.mocked(fetchMyServers).mockResolvedValue({ ok: true, value: serversResponse([hostedServer(4), hostedServer(5, { state: 'stopped' })]) });
    vi.mocked(listMyServers).mockResolvedValue({
      ok: true,
      value: [listedServer(1, { hosted_server_id: 4, players: 5 }), listedServer(2, { players: 1 })],
    });
    const view = await render();
    expect(tile(view, 'Servers').get('dd').text()).toBe('2 live');
    expect(tile(view, 'Servers').text()).toContain('6 players on them');
  });

  it('shows the allowance with none live, and offers no start without one', async () => {
    const view = await render();
    expect(tile(view, 'Servers').text()).toContain('0 of 3 in use');
    page?.unmount();

    vi.mocked(fetchMyServers).mockResolvedValue({ ok: true, value: serversResponse([], { entitlement: { max_servers: 0, can_start: false } }) });
    const locked = await render();
    expect(tile(locked, 'Servers').text()).toContain('No hosting yet');
    expect(locked.find('a[href="/me/servers/new"]').exists()).toBe(false);
  });

  it('counts the projects and the published ones', async () => {
    const view = await render();
    expect(tile(view, 'Projects').get('dd').text()).toBe('2');
    expect(tile(view, 'Projects').text()).toContain('1 published');
  });

  it("names the account's tier, an admin grant, or none", async () => {
    vi.mocked(fetchMyBilling).mockResolvedValue({ ok: true, value: subscribed(2) });
    const tier = await render();
    expect(tile(tier, 'Billing').get('dd').text()).toBe('Tier 2');
    expect(tile(tier, 'Billing').text()).toMatch(/Active, renews/);
    page?.unmount();

    vi.mocked(fetchMyBilling).mockResolvedValue({ ok: true, value: myBilling({ granted: true, comp_servers: 2 }) });
    const granted = await render();
    expect(tile(granted, 'Billing').get('dd').text()).toBe('Admin grant');
    page?.unmount();

    vi.mocked(fetchMyBilling).mockResolvedValue({ ok: true, value: myBilling() });
    const none = await render();
    expect(tile(none, 'Billing').get('dd').text()).toBe('No tier');
  });

  it('says Billing off when billing answers disabled, with no error', async () => {
    vi.mocked(fetchMyBilling).mockResolvedValue({ ok: false, error: { code: 'billing_disabled', status: 503, message: 'Support tiers are not open on NovaWorld yet.' } });
    const view = await render();
    expect(tile(view, 'Billing').get('dd').text()).toBe('Billing off');
    expect(view.find('[role="alert"]').exists()).toBe(false);
    expect(view.get('.needs-you-empty').text()).toBe('Nothing needs you.');
  });

  it('keeps the other tiles when one read fails, and never says nothing needs you then', async () => {
    vi.mocked(fetchMyServers).mockResolvedValue({ ok: false, error: { code: 'network', status: null, message: 'Could not reach the server.' } });
    vi.mocked(fetchMyBilling).mockResolvedValue({ ok: true, value: subscribed(2) });
    const view = await render();
    expect(view.get('[role="alert"]').text()).toBe('Could not read your servers: Could not reach the server.');
    expect(tile(view, 'Servers').get('dd').text()).toBe('Not loaded');
    expect(tile(view, 'Billing').get('dd').text()).toBe('Tier 2');
    expect(view.find('.needs-you-empty').exists()).toBe(false);
  });

  it('puts a payment past due first, linked to billing', async () => {
    const account = subscribed(1);
    account.subscription!.status = 'past_due';
    vi.mocked(fetchMyBilling).mockResolvedValue({ ok: true, value: account });
    const view = await render();
    expect(needs(view)[0]).toEqual({ badge: 'Act now', subject: 'Billing', link: '/me/billing', text: 'A payment did not go through.' });
    expect(tile(view, 'Billing').classes()).toContain('border-warn/40');
  });

  it('lists a server that failed in the last day, and not an older or replaced failure', async () => {
    vi.mocked(fetchMyServers).mockResolvedValue({
      ok: true,
      value: serversResponse([
        hostedServer(9, { state: 'failed', failure_reason: 'the launch failed', ended_at: ago(2 * HOUR), settings: { ...hostedServer(9).settings, game_name: 'Night Raid' } }),
        hostedServer(8, { state: 'failed', ended_at: ago(30 * HOUR) }),
        hostedServer(6, { state: 'failed', ended_at: ago(HOUR), settings: { ...hostedServer(6).settings, game_name: 'Again' } }),
        hostedServer(7, { settings: { ...hostedServer(7).settings, game_name: 'Again' } }),
      ]),
    });
    const view = await render();
    const rows = needs(view);
    expect(rows).toHaveLength(1);
    expect(rows[0]).toEqual({ badge: 'Act now', subject: 'Night Raid', link: '/me/servers/9', text: 'Failed: the launch failed' });
    expect(view.get('.needs-you-item time').text()).toBe('2 hours ago');
  });

  it('lists a live server with an update not asked for yet', async () => {
    vi.mocked(fetchMyServers).mockResolvedValue({
      ok: true,
      value: serversResponse([hostedServer(4, { update_available: true }), hostedServer(5, { update_available: true, update_requested: true })]),
    });
    const view = await render();
    expect(needs(view)).toEqual([{ badge: 'Look soon', subject: 'Server 4', link: '/me/servers/4', text: 'Update available.' }]);
  });

  it('lists static addresses bought and not named yet', async () => {
    vi.mocked(fetchMyAddresses).mockResolvedValue({ ok: true, value: addressesResponse([], { unnamed: 2 }) });
    vi.mocked(fetchMyBilling).mockResolvedValue({ ok: true, value: myBilling({ address_subscriptions: [addressSubscription({ address_id: null })] }) });
    const view = await render();
    expect(needs(view)).toEqual([{ badge: 'Look soon', subject: 'Static addresses', link: '/me/billing#addresses', text: '2 wait for a name.' }]);
  });

  it('lists a release sent back, then one awaiting approval, reading only the projects it may release', async () => {
    releases.mockResolvedValue({
      ok: true,
      value: [
        nightOpsRelease(5, { status: 'awaiting_approval' }),
        nightOpsRelease(4, { review_note: 'The briefing names the wrong map.' }),
        nightOpsRelease(3),
      ],
    });
    const view = await render();
    // RANGERS has nothing pushed: no release to read.
    expect(releases).toHaveBeenCalledTimes(1);
    expect(releases).toHaveBeenCalledWith('NIGHTOPS', expect.anything());
    expect(needs(view)).toEqual([
      { badge: 'Look soon', subject: 'Night Ops', link: '/me/projects/NIGHTOPS', text: '0.4.0 sent back: The briefing names the wrong map.' },
      { badge: 'Note', subject: 'Night Ops', link: '/me/projects/NIGHTOPS', text: '0.5.0 awaits approval.' },
    ]);
  });

  it('sends an ended session to the login', async () => {
    vi.mocked(fetchMyProjects).mockResolvedValue({ ok: false, error: { code: 'unauthorized', status: 401, field: '', message: 'ended' } });
    await render();
    expect(refreshSession).toHaveBeenCalled();
    expect(router.currentRoute.value.fullPath).toBe('/login?redirect=/me');
  });
});
