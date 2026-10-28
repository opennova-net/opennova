import * as actionReview from '../../../composables/useConfirm';
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import { createMemoryHistory, createRouter, RouterView } from 'vue-router';
import PlayerPage from './PlayerPage.vue';
import { addCompAddress } from '../../../api/adminAddresses';
import { fetchAccountCdKeys } from '../../../api/cdKeys';
import { fetchOnline } from '../../../api/online';
import { fetchProducts } from '../../../api/products';
import { deleteUser, fetchUser, listUsers, setCompGrants, updateUser } from '../../../api/users';
import { adminAddress } from '../../../test/addressFixtures';
import { user, userDetail, userEntry } from '../../../test/adminFixtures';
import { hostedServer } from '../../../test/hostedFixtures';
import { onlinePlayer, onlineResponse } from '../../../test/onlineFixtures';
import { JO_PRODUCT, product, productsResponse } from '../../../test/productFixtures';
import { testRouter } from '../../../test/router';
import type { HostedServer } from '../../../types/hostedServer';
import type { SoldCapacity } from '../../../types/users';

vi.mock('../../../api/users', () => ({
  fetchUser: vi.fn(),
  listUsers: vi.fn(),
  updateUser: vi.fn(),
  setCompGrants: vi.fn(),
  deleteUser: vi.fn(),
}));
vi.mock('../../../api/cdKeys', () => ({ fetchAccountCdKeys: vi.fn() }));
vi.mock('../../../api/adminAddresses', () => ({ addCompAddress: vi.fn() }));
vi.mock('../../../api/online', () => ({ fetchOnline: vi.fn() }));
vi.mock('../../../api/products', () => ({ fetchProducts: vi.fn() }));

const detail = vi.mocked(fetchUser);
const list = vi.mocked(listUsers);
const update = vi.mocked(updateUser);
const comp = vi.mocked(setCompGrants);
const remove = vi.mocked(deleteUser);
const keys = vi.mocked(fetchAccountCdKeys);
const addAddress = vi.mocked(addCompAddress);
const online = vi.mocked(fetchOnline);
const products = vi.mocked(fetchProducts);

// Shared places sold past the fleet: an admin's grant took them there.
const OVERSOLD: SoldCapacity = {
  shared: { sold: 26, sellable: 24 },
  own_machines: { sold: 4, sellable: 4 },
  addresses: { sold: 1, sellable: 4 },
  oversold: true,
};

const LISTING = { rid: 167772161, server_name: 'Server 31', mission_name: 'River Patrol', game_type: 'TDM', players: 5, max_players: 32, host_ip: '203.0.113.7', host_port: 32768 };

// player7: two live servers and one ended, a tier 2 subscription, one key,
// a project of his own, playing on hosted server 31 now.
function account() {
  return userDetail(7, {
    user: user(7, { max_servers: 2, created_at: '2026-09-14T12:00:00Z', last_login: new Date(Date.now() - 2 * 3_600_000).toISOString() }),
    servers: [hostedServer(31, { listing: LISTING }), hostedServer(32), hostedServer(30, { state: 'stopped' })],
    billing: {
      livemode: false,
      stripe_customer_id: 'cus_test_7',
      subscriptions: [
        {
          stripe_subscription_id: 'sub_test_7',
          item: 'tier',
          tier: 2,
          quantity: 1,
          status: 'active',
          entitled: true,
          current_period_end: '2026-11-14T12:00:00Z',
          cancel_at_period_end: false,
        },
      ],
    },
  });
}

async function mountPage(path = '/admin/players/7') {
  const router = await testRouter(path, ['/admin/players/:id']);
  const wrapper = mount(PlayerPage, { global: { plugins: [router] } });
  await flushPromises();
  return { wrapper, router };
}

function section(wrapper: VueWrapper, heading: string) {
  return wrapper.get(`section[aria-labelledby="${heading}"]`);
}

describe('PlayerPage', () => {
  let confirm: MockInstance<typeof actionReview.confirmAction>;

  beforeEach(() => {
    vi.clearAllMocks();
    confirm = vi.spyOn(actionReview, 'confirmAction').mockResolvedValue(true);
    detail.mockResolvedValue({ ok: true, value: account() });
    keys.mockResolvedValue({
      ok: true,
      value: {
        account: { id: 7, username: 'player7' },
        keys: [
          {
            id: 3,
            title: 'dfx2',
            serial: 'DFX2-0000-0000-0000-0003',
            wire_key: 'X',
            state: 'assigned',
            account: { id: 7, username: 'player7' },
            created_by: 'auto',
            created_at: '2026-10-14 12:00:00',
            assigned_at: '2026-10-14 12:00:00',
            revoked_at: null,
            activations: 2,
          },
        ],
        activations: [],
      },
    });
    online.mockResolvedValue({
      ok: true,
      value: onlineResponse([
        onlinePlayer('Player7', {
          account: { id: 7, username: 'player7', nwhandle: 'Player7' },
          server: { rid: 167772161, name: 'Server 31', mission: 'River Patrol', game_type: 'TDM', hosted_server_id: 31 },
        }),
        onlinePlayer('Other', { account: { id: 8, username: 'other', nwhandle: 'Other' } }),
      ]),
    });
    products.mockResolvedValue({
      ok: true,
      value: productsResponse({
        products: [JO_PRODUCT, product(20, { display_name: 'Night Ops', key: 'NIGHTOPS', owner: 'player7', owner_id: 7, published_release: { id: 4, name: '0.2.0', sha256: 'ab' } }), product(21, { owner: 'other', owner_id: 8 })],
      }),
    });
  });

  afterEach(() => confirm.mockRestore());

  it('heads the page with the account, where it plays now, and its tabs', async () => {
    const { wrapper } = await mountPage();
    expect(detail).toHaveBeenCalledWith(7);
    expect(wrapper.get('h1').text()).toBe('player7');
    expect(wrapper.get('.page-crumbs a').attributes('href')).toBe('/admin/players/accounts');
    const meta = wrapper.get('.page-meta');
    expect(meta.get('.player-handle').text()).toBe('Player7');
    expect(meta.text()).toContain('Player');
    expect(meta.text()).toContain('Active');
    expect(meta.text()).toContain('player7@example.com');
    expect(meta.get('.player-online').text()).toContain('Online');
    expect(meta.get('.player-online a').attributes('href')).toBe('/admin/servers/31');
    expect(meta.get('.player-joined').text()).toMatch(/^Joined .*2026/);
    expect(meta.get('.player-seen').text()).toBe('Last seen 2 hours ago');
    const tabs = wrapper.findAll('.tab-nav a');
    expect(tabs.map((a) => a.text())).toEqual(['Overview', 'Settings']);
    expect(tabs[0].attributes('aria-current')).toBe('page');
    expect(tabs[1].attributes('href')).toBe('/admin/players/7?tab=settings');
    // The email is read-only: no field edits it.
    expect(wrapper.findAll('input').some((i) => (i.element as HTMLInputElement).value === 'player7@example.com')).toBe(false);
  });

  it('says nothing of online for an account that is not, and never signed in', async () => {
    detail.mockResolvedValue({ ok: true, value: userDetail(7) });
    online.mockResolvedValue({ ok: true, value: onlineResponse([]) });
    products.mockResolvedValue({ ok: false, error: { code: 'network', status: null, message: 'Could not reach the server.' } });
    const { wrapper } = await mountPage();
    expect(wrapper.find('.player-online').exists()).toBe(false);
    expect(wrapper.find('.player-joined').exists()).toBe(false);
    expect(wrapper.get('.player-seen').text()).toBe('Last seen never');
    // The projects could not be read: the rest still shows.
    expect(section(wrapper, 'player-projects').get('tbody').text()).toBe('Not loaded.');
    expect(wrapper.get('[role="alert"]').text()).toBe('');
  });

  it('shows its servers, projects, billing, keys and addresses on the Overview, every one linked', async () => {
    detail.mockResolvedValue({
      ok: true,
      value: {
        ...account(),
        addresses: [
          adminAddress(1, 'smoke', { server: { id: 31, game_name: 'Night Ops', state: 'running' } }),
          adminAddress(2, 'spare', { source: 'comp', stripe_subscription_id: null, state: 'allocating', public_ip: null, port: null }),
        ],
      },
    });
    const { wrapper } = await mountPage();

    const servers = section(wrapper, 'player-servers');
    const rows = servers.findAll('tr.player-server');
    expect(rows.map((r) => r.get('.server-link a').attributes('href'))).toEqual(['/admin/servers/31', '/admin/servers/32', '/admin/servers/30']);
    expect(rows[0].text()).toContain('Joint Operations');
    expect(rows[0].text()).toContain('Running');
    expect(rows[0].text()).toContain('5 of 32');
    expect(servers.findAll('a').find((a) => a.text() === 'See in Servers')!.attributes('href')).toBe('/admin/servers?owner=player7');

    const projects = section(wrapper, 'player-projects').findAll('tr.player-project');
    expect(projects).toHaveLength(1);
    expect(projects[0].get('.product-link').attributes('href')).toBe('/admin/products/NIGHTOPS');
    expect(projects[0].text()).toContain('Game');
    expect(projects[0].text()).toContain('0.2.0');

    const billing = section(wrapper, 'player-billing');
    expect(billing.get('dl').text()).toContain('Tier 2');
    expect(billing.get('dl').text()).toContain('2 servers: 2 on shared machines');
    expect(billing.get('dl').text()).toContain('May start');
    const customer = billing.findAll('a').find((a) => a.text() === 'cus_test_7')!;
    expect(customer.attributes('href')).toBe('https://dashboard.stripe.com/test/customers/cus_test_7');
    expect(customer.attributes('rel')).toContain('noopener');
    expect(billing.get('table').text()).toContain('sub_test_7');

    const cdKeys = section(wrapper, 'player-cd-keys');
    expect(cdKeys.get('tr.player-key').text()).toContain('DFX2-0000-0000-0000-0003');
    expect(cdKeys.get('tr.player-key').text()).toContain('Delta Force Xtreme 2');
    expect(cdKeys.findAll('a').find((a) => a.text() === 'Manage keys')!.attributes('href')).toBe('/admin/players/cd-keys?account=7#cd-account-heading');

    const addresses = section(wrapper, 'player-addresses');
    const [smoke, spare] = addresses.findAll('tr.player-address');
    expect(smoke.text()).toContain('smoke.play.opennova.net');
    expect(smoke.text()).toContain('203.0.113.1:32768');
    expect(smoke.text()).toContain('Subscription');
    expect(smoke.get('.server-link a').attributes('href')).toBe('/admin/servers/31');
    expect(spare.text()).toContain('No IP yet');
    expect(spare.text()).toContain('Admin grant');
    expect(spare.text()).toContain('Being set up');

    // The Settings forms are on their tab.
    expect(wrapper.find('#player-profile').exists()).toBe(false);
    expect(wrapper.find('#user-comp-servers').exists()).toBe(false);
  });

  it('keeps the profile, role, grant, comp address and deletion in Settings', async () => {
    const { wrapper, router } = await mountPage();
    await router.push('/admin/players/7?tab=settings');
    await flushPromises();
    expect(wrapper.findAll('.tab-nav a')[1].attributes('aria-current')).toBe('page');
    expect(wrapper.find('#player-servers').exists()).toBe(false);
    expect(wrapper.findAll('h2').map((h) => h.text())).toEqual(['Profile', 'Role', 'Hosting grant', 'Comp static address', 'Delete account']);
    expect(wrapper.get('details summary').text()).toBe('Advanced identity');
    expect((wrapper.get('#player-username').element as HTMLInputElement).value).toBe('player7');
    expect((wrapper.get('#player-pcid').element as HTMLInputElement).value).toBe('0000ABCD');

    update.mockResolvedValue({ ok: true, value: user(7, { nwhandle: 'Seven' }) });
    await wrapper.get('#player-handle').setValue('Seven');
    await wrapper.get('#player-handle').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(update).toHaveBeenCalledWith(7, { username: 'player7', nwhandle: 'Seven', pcid: '0000ABCD', nwh: '1' });
    expect(wrapper.get('[role="status"]').text()).toBe('Profile saved.');
  });

  it('grants shared servers and dedicated machines from 0 to 16', async () => {
    const { wrapper } = await mountPage('/admin/players/7?tab=settings');
    comp.mockResolvedValue({ ok: true, value: { user: user(7, { comp_servers: 3, comp_own_machines: 1, max_servers: 3 }), capacity: null } });
    await wrapper.get('#user-comp-servers').setValue(17);
    expect(wrapper.get('#user-comp-problem').text()).toBe('Enter a whole number from 0 to 16.');
    expect(wrapper.get('#user-comp-servers').attributes('aria-invalid')).toBe('true');
    await wrapper.get('#user-comp-servers').setValue(3);
    await wrapper.get('#user-comp-own').setValue(-1);
    expect(wrapper.get('#user-comp-problem').text()).toBe('Enter a whole number from 0 to 16.');
    await wrapper.get('#user-comp-own').setValue(1);
    expect(wrapper.find('#user-comp-problem').exists()).toBe(false);
    await wrapper.get('#user-comp-servers').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(confirm).not.toHaveBeenCalled();
    expect(comp).toHaveBeenCalledWith(7, { servers: 3, own_machines: 1 });
    expect(wrapper.get('[role="status"]').text()).toBe("player7's grant is now 3 shared servers and 1 dedicated machine.");

    // A tier of 2 servers, 2 live: a grant of 0 stops none; the tier still covers them.
    comp.mockClear();
    await wrapper.get('#user-comp-servers').setValue(0);
    await wrapper.get('#user-comp-own').setValue(0);
    await wrapper.get('#user-comp-servers').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(confirm).not.toHaveBeenCalled();
    expect(comp).toHaveBeenCalledWith(7, { servers: 0, own_machines: 0 });
  });

  it('warns while grants and subscriptions oversell the fleet, also right after a grant', async () => {
    const { wrapper } = await mountPage('/admin/players/7?tab=settings');
    expect(wrapper.find('#user-oversold').exists()).toBe(false);
    comp.mockResolvedValue({ ok: true, value: { user: user(7, { comp_servers: 6, max_servers: 6 }), capacity: OVERSOLD } });
    detail.mockResolvedValueOnce({ ok: false, error: { code: 'network', status: null, message: 'Could not reach the server.' } });
    await wrapper.get('#user-comp-servers').setValue(6);
    await wrapper.get('#user-comp-servers').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(wrapper.get('#user-oversold').text()).toBe(
      'Sold: 26 of 24 shared places, 4 of 4 dedicated machines, 1 of 4 static addresses. New subscriptions are paused.',
    );

    detail.mockResolvedValue({ ok: true, value: userDetail(7, { capacity: OVERSOLD }) });
    const again = await mountPage();
    expect(again.wrapper.get('#user-oversold').text()).toContain('26 of 24 shared places');
  });

  // An account with an admin grant of `shared` servers on shared machines and
  // `own` dedicated machines, and these servers.
  function granted(shared: number, own: number, servers: HostedServer[]) {
    return userDetail(7, {
      user: user(7, { comp_servers: shared, comp_own_machines: own, max_servers: shared + own }),
      entitlement: {
        max_servers: shared + own,
        shared_servers: shared,
        source: shared > 0 ? 'comp' : 'none',
        tier: null,
        comp_servers: shared,
        comp_own_machines: own,
        tier_servers: 0,
        own_machines: own,
        can_start: true,
      },
      servers,
    });
  }

  async function submitGrant(wrapper: VueWrapper, shared: number, own: number) {
    await wrapper.get('#user-comp-servers').setValue(shared);
    await wrapper.get('#user-comp-own').setValue(own);
    await wrapper.get('#user-comp-servers').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
  }

  it('confirms a cut of shared servers below the ones running there', async () => {
    // 3 shared + 1 dedicated, all four live: the dedicated one is no part of the cut.
    detail.mockResolvedValue({ ok: true, value: granted(3, 1, [hostedServer(30, { own_machine: true }), hostedServer(31), hostedServer(32), hostedServer(33)]) });
    const { wrapper } = await mountPage('/admin/players/7?tab=settings');
    confirm.mockResolvedValueOnce(false);
    await submitGrant(wrapper, 1, 1);
    expect(confirm).toHaveBeenCalledWith("Lower player7's shared servers to 1? Their 2 newest servers there stop, ending their matches.");
    expect(comp).not.toHaveBeenCalled();
  });

  it('confirms a cut of dedicated machines: their servers move into free shared servers, else stop', async () => {
    // 3 shared + 2 dedicated, one shared server live: two shared servers free.
    const servers = [hostedServer(31, { own_machine: true }), hostedServer(32, { own_machine: true }), hostedServer(33)];
    detail.mockResolvedValue({ ok: true, value: granted(3, 2, servers) });
    const { wrapper } = await mountPage('/admin/players/7?tab=settings');
    confirm.mockResolvedValue(false);
    await submitGrant(wrapper, 3, 0);
    expect(confirm).toHaveBeenLastCalledWith(
      "Lower player7's dedicated machines to 0? Their 2 newest servers on them move to a shared machine, restarting their matches, or stop if the fleet is full.",
    );
    await submitGrant(wrapper, 3, 1);
    expect(confirm).toHaveBeenLastCalledWith(
      "Lower player7's dedicated machines to 1? Their newest server on one moves to a shared machine, restarting its match, or stops if the fleet is full.",
    );
    // Down to one shared server too: the live one holds it, so none is free.
    await submitGrant(wrapper, 1, 0);
    expect(confirm).toHaveBeenLastCalledWith("Lower player7's dedicated machines to 0? Their 2 newest servers on them stop.");
    // Two shared servers, one free: one moves, one stops.
    await submitGrant(wrapper, 2, 0);
    expect(confirm).toHaveBeenLastCalledWith(
      "Lower player7's dedicated machines to 0? Of their 2 newest servers on them, 1 moves to a shared machine and the rest stop.",
    );
    expect(comp).not.toHaveBeenCalled();

    // Both pools cut at once: one confirm says both.
    await submitGrant(wrapper, 0, 0);
    const both = confirm.mock.lastCall![0] as string;
    expect(both).toContain("Lower player7's shared servers to 0? Their newest server there stops, ending its match.");
    expect(both).toContain("Lower player7's dedicated machines to 0? Their 2 newest servers on them stop.");
  });

  it('confirms nothing for a cut the servers fit in, counts the tier under a grant and no stopping server', async () => {
    const servers = [hostedServer(31, { own_machine: true }), hostedServer(32, { own_machine: true }), hostedServer(33)];
    detail.mockResolvedValue({ ok: true, value: granted(3, 2, servers) });
    comp.mockResolvedValue({ ok: true, value: { user: user(7, { comp_servers: 1, comp_own_machines: 2, max_servers: 3 }), capacity: null } });
    const { wrapper } = await mountPage('/admin/players/7?tab=settings');
    await submitGrant(wrapper, 1, 2);
    expect(confirm).not.toHaveBeenCalled();
    expect(comp).toHaveBeenCalledWith(7, { servers: 1, own_machines: 2 });
    wrapper.unmount();

    // Tier 1 (one server) beside a grant of 3, of three live servers one stopping.
    comp.mockClear();
    detail.mockResolvedValue({
      ok: true,
      value: userDetail(7, {
        user: user(7, { comp_servers: 3, max_servers: 4 }),
        entitlement: { max_servers: 4, shared_servers: 4, source: 'tier', tier: 1, comp_servers: 3, comp_own_machines: 0, tier_servers: 1, own_machines: 0, can_start: true },
        servers: [hostedServer(31), hostedServer(32), hostedServer(33, { state: 'stopping' })],
      }),
    });
    const tiered = await mountPage('/admin/players/7?tab=settings');
    confirm.mockResolvedValueOnce(false);
    await submitGrant(tiered.wrapper, 0, 0);
    expect(confirm).toHaveBeenCalledWith("Lower player7's shared servers to 1? Their newest server there stops, ending its match.");
    expect(comp).not.toHaveBeenCalled();
  });

  it('adds a comp address, checking its name first, and drops the form when no address is sold', async () => {
    const { wrapper } = await mountPage('/admin/players/7?tab=settings');
    await wrapper.get('#user-comp-address').setValue('Bad--Name');
    await wrapper.get('#user-comp-address').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(addAddress).not.toHaveBeenCalled();
    expect(wrapper.get('#user-comp-address-problem').text()).toContain('3 to 32 lower-case letters');
    expect(wrapper.get('#user-comp-address').attributes('aria-invalid')).toBe('true');

    addAddress.mockResolvedValueOnce({ ok: true, value: adminAddress(3, 'arena', { state: 'allocating' }) });
    await wrapper.get('#user-comp-address').setValue('arena');
    await wrapper.get('#user-comp-address').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(addAddress).toHaveBeenCalledWith(7, 'arena');
    expect(wrapper.get('[role="status"]').text()).toBe('player7 has the comp address arena.play.opennova.net.');

    addAddress.mockResolvedValueOnce({ ok: false, error: { code: 'addresses_disabled', status: 503, message: 'This NovaWorld service sells no static address.' } });
    await wrapper.get('#user-comp-address').setValue('arena2');
    await wrapper.get('#user-comp-address').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(wrapper.get('[role="alert"]').text()).toContain('sells no static address');
    expect(wrapper.find('#user-comp-address').exists()).toBe(false);
  });

  it('changes the role after a confirmation judged on every admin', async () => {
    list.mockResolvedValue({ ok: true, value: [userEntry(7), userEntry(8, { role: 'admin' })] });
    update.mockResolvedValue({ ok: true, value: user(7, { role: 'admin' }) });
    const { wrapper } = await mountPage('/admin/players/7?tab=settings');
    await wrapper.findAll('button').find((b) => b.text() === 'Make admin')!.trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenCalledWith('Make player7 an admin? They can manage every account and the service.');
    expect(update).toHaveBeenCalledWith(7, { role: 'admin' });
    expect(wrapper.get('[role="status"]').text()).toBe('player7 is now an admin.');

    // Demoting the last admin who can sign in says so.
    list.mockResolvedValue({ ok: true, value: [userEntry(7, { role: 'admin' }), userEntry(8, { role: 'admin', account_status: 'banned' })] });
    confirm.mockResolvedValueOnce(false);
    await wrapper.findAll('button').find((b) => b.text() === 'Make player')!.trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenLastCalledWith(
      'Make player7 a player? They lose the admin pages at once. No other admin can sign in: only the admin token can make one again.',
    );
  });

  it('refuses to delete while a subscription grants, in words, and leaves for the accounts once deleted', async () => {
    remove.mockResolvedValue({ ok: false, error: { code: 'active_subscription', status: 409, message: 'This account has a subscription that still grants.' } });
    const { wrapper, router } = await mountPage('/admin/players/7?tab=settings');
    const button = wrapper.findAll('button').find((b) => b.text().startsWith('Delete'))!;
    await button.trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenCalledWith('Delete player7? This cannot be undone.');
    expect(remove).toHaveBeenCalledWith(7);
    expect(wrapper.get('[role="alert"]').text()).toContain('a subscription that still grants');
    expect(router.currentRoute.value.path).toBe('/admin/players/7');

    remove.mockResolvedValue({ ok: true, value: undefined });
    await button.trigger('click');
    await flushPromises();
    expect(router.currentRoute.value.path).toBe('/admin/players/accounts');
  });

  it('keeps drafts after a failed deletion and leaves a deleted account without an unsaved warning', async () => {
    const router = createRouter({
      history: createMemoryHistory(),
      routes: [
        { path: '/admin/players/:id', component: PlayerPage },
        { path: '/:rest(.*)*', component: { render: () => null } },
      ],
    });
    await router.push('/admin/players/7?tab=settings');
    const page = mount(RouterView, { global: { plugins: [router] } });
    try {
      await flushPromises();
      await page.get('#player-username').setValue('unsaved-name');
      remove.mockResolvedValueOnce({ ok: false, error: { code: 'active_subscription', status: 409, message: 'Subscription still grants.' } });
      const deleteButton = () => page.findAll('button').find((button) => button.text().startsWith('Delete'))!;
      await deleteButton().trigger('click');
      await flushPromises();
      confirm.mockResolvedValue(false);
      await router.push('/admin/players/accounts');
      expect(router.currentRoute.value.path).toBe('/admin/players/7');
      expect(confirm).toHaveBeenLastCalledWith(expect.stringContaining('unsaved changes'), 'Unsaved changes');

      remove.mockResolvedValueOnce({ ok: true, value: undefined });
      confirm.mockClear();
      confirm.mockResolvedValueOnce(true);
      await deleteButton().trigger('click');
      await flushPromises();
      expect(router.currentRoute.value.path).toBe('/admin/players/accounts');
      expect(confirm).toHaveBeenCalledTimes(1);
    } finally {
      page.unmount();
    }
  });

  it('says when the account cannot be read, and reads it again on Try again', async () => {
    detail.mockResolvedValueOnce({ ok: false, error: { code: 'not_found', status: 404, message: 'Not found.' } });
    const { wrapper } = await mountPage();
    expect(wrapper.get('h1').text()).toBe('Player');
    expect(wrapper.get('[role="alert"]').text()).toBe('No account has this id.');
    await wrapper.findAll('button').find((b) => b.text() === 'Try again')!.trigger('click');
    await flushPromises();
    expect(wrapper.get('h1').text()).toBe('player7');
    expect(wrapper.get('[role="alert"]').text()).toBe('');
  });
});
