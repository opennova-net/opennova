import { flushPromises, mount } from '@vue/test-utils';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import AccountsTab from './AccountsTab.vue';
import { fetchOnline } from '../../../api/online';
import type { SessionUser } from '../../../api/session';
import { refreshSession } from '../../../composables/useSession';
import { createUserAdmin, listUsers, registerUser } from '../../../api/users';
import { userEntry } from '../../../test/adminFixtures';
import { onlinePlayer, onlineResponse } from '../../../test/onlineFixtures';
import { testRouter } from '../../../test/router';
import type { UserEntitlementSummary } from '../../../types/users';

vi.mock('../../../api/users', () => ({ listUsers: vi.fn(), createUserAdmin: vi.fn(), registerUser: vi.fn() }));
vi.mock('../../../api/online', () => ({ fetchOnline: vi.fn() }));

// The session /api/me answers: biggy, who sees billing, on an account the
// list does not hold, unless a test says.
const BIGGY: SessionUser = { id: 900, username: 'biggy', role: 'admin' };
const session = vi.hoisted(() => ({ user: null as SessionUser | null }));
vi.mock('../../../api/session', async (original) => ({
  ...(await original<typeof import('../../../api/session')>()),
  me: vi.fn(async () => ({ ok: true, value: session.user })),
}));

const list = vi.mocked(listUsers);
const online = vi.mocked(fetchOnline);
const create = vi.mocked(createUserAdmin);
const register = vi.mocked(registerUser);

// A users list row's entitlement: none, unless the overrides say otherwise.
function summary(overrides: Partial<UserEntitlementSummary> = {}): UserEntitlementSummary {
  return { max_servers: 0, shared_servers: 0, source: 'none', tier: null, comp_servers: 0, own_machines: 0, comp_own_machines: 0, ...overrides };
}

const USERS = [
  userEntry(1, {
    username: 'alpha',
    nwhandle: 'Alpha Dog',
    max_servers: 2,
    entitlement: summary({ max_servers: 2, shared_servers: 2, source: 'tier', tier: 2 }),
    live_servers: 1,
    subscription_status: 'active',
    created_at: '2026-09-01T12:00:00Z',
    last_login: new Date(Date.now() - 3 * 3_600_000).toISOString(),
  }),
  userEntry(2, {
    username: 'bravo',
    nwhandle: 'bravo',
    role: 'admin',
    comp_servers: 3,
    max_servers: 3,
    entitlement: summary({ max_servers: 3, shared_servers: 3, source: 'comp', comp_servers: 3 }),
  }),
  userEntry(3, {
    username: 'charlie',
    entitlement: summary({ max_servers: 1, shared_servers: 1, source: 'tier', tier: 1 }),
    live_servers: 2,
    subscription_status: 'past_due',
    account_status: 'banned',
  }),
  // A dedicated machine paid for, no tier.
  userEntry(4, { username: 'delta', max_servers: 1, entitlement: summary({ max_servers: 1, own_machines: 1 }) }),
];

async function mountTab(path = '/admin/players/accounts') {
  const router = await testRouter(path);
  const wrapper = mount(AccountsTab, { global: { plugins: [router] } });
  await flushPromises();
  return { wrapper, router };
}

function shownNames(wrapper: Awaited<ReturnType<typeof mountTab>>['wrapper']) {
  return wrapper.findAll('tr.user-row').map((r) => r.get('.player-link').text());
}

describe('AccountsTab', () => {
  beforeEach(async () => {
    vi.clearAllMocks();
    session.user = BIGGY;
    await refreshSession();
    list.mockResolvedValue({ ok: true, value: USERS });
    online.mockResolvedValue({ ok: true, value: onlineResponse([onlinePlayer('Alpha Dog', { account: { id: 1, username: 'alpha', nwhandle: 'Alpha Dog' } })]) });
  });

  it('lists each account with its role, tier, subscription, servers, last sign-in and whether it is online', async () => {
    const { wrapper } = await mountTab();
    expect(wrapper.get('h1').text()).toBe('Players');
    expect(wrapper.get('.accounts-count').text()).toBe('4 accounts');
    expect(wrapper.findAll('thead th').map((th) => th.text())).toEqual([
      'Account',
      'Role',
      'Support tier',
      'Subscription',
      'Servers',
      'Last seen',
      'Online',
      'Status',
    ]);
    const [alpha, bravo, charlie, delta] = wrapper.findAll('tr.user-row');

    expect(alpha.get('.player-link').attributes('href')).toBe('/admin/players/1');
    expect(alpha.get('th').text()).toContain('player1@example.com');
    // The in-game handle when it differs from the username, else not twice.
    expect(alpha.get('.user-handle').text()).toBe('Alpha Dog');
    expect(bravo.find('.user-handle').exists()).toBe(false);
    expect(alpha.get('.user-tier').text()).toBe('Tier 2');
    expect(alpha.get('.user-tier .badge').classes()).toContain('badge-info');
    expect(alpha.text()).toContain('Active');
    const servers = alpha.get('.user-servers a');
    expect(servers.text()).toBe('1 live');
    expect(servers.attributes('href')).toBe('/admin/servers?owner=alpha');
    expect(alpha.text()).toContain('3 hours ago');
    expect(alpha.get('.user-online').text()).toBe('Online');

    expect(bravo.text()).toContain('Admin');
    expect(bravo.get('.user-tier').text()).toBe('Admin grant of 3 shared');
    expect(bravo.get('.user-tier .badge').classes()).toContain('badge-ok');
    expect(bravo.get('.user-servers').text()).toBe('None');
    expect(bravo.get('.user-servers').find('a').exists()).toBe(false);
    expect(bravo.text()).toContain('Never');
    expect(bravo.get('.user-online').text()).toBe('');

    expect(charlie.text()).toContain('Payment past due');
    // More live than it may run: marked.
    expect(charlie.get('.user-servers').text()).toBe('2 live over 1');
    expect(charlie.findAll('.badge').at(-1)!.text()).toBe('Banned');
    expect(charlie.findAll('.badge').at(-1)!.classes()).toContain('badge-bad');

    expect(delta.get('.user-tier').text()).toBe('1 dedicated');
    expect(wrapper.text()).not.toContain('—');
  });

  it('shows another admin no Subscription column', async () => {
    session.user = { id: 901, username: 'ghost', role: 'admin' };
    await refreshSession();
    const { wrapper } = await mountTab();
    expect(wrapper.findAll('thead th').map((th) => th.text())).toEqual(['Account', 'Role', 'Support tier', 'Servers', 'Last seen', 'Online', 'Status']);
    const charlie = wrapper.findAll('tr.user-row')[2];
    expect(charlie.findAll('td')).toHaveLength(6);
    expect(charlie.text()).not.toContain('Payment past due');
  });

  it('filters by search, role and status in the URL query, and restores them from it', async () => {
    const { wrapper, router } = await mountTab('/admin/players/accounts?q=biggy');
    expect(wrapper.get('tbody').text()).toBe('No account matches.');
    await wrapper.get('#users-filter').setValue('BRA');
    await flushPromises();
    expect(shownNames(wrapper)).toEqual(['bravo']);
    for (const q of ['alpha dog', 'player3@EXAMPLE.com', '4']) {
      await wrapper.get('#users-filter').setValue(q);
      await flushPromises();
      expect(shownNames(wrapper)).toHaveLength(1);
    }
    await wrapper.get('#users-filter').setValue('');
    await flushPromises();
    await wrapper.get('#users-role').setValue('admin');
    await flushPromises();
    expect(shownNames(wrapper)).toEqual(['bravo']);
    expect(wrapper.get('.accounts-count').text()).toBe('1 of 4 accounts');
    await wrapper.get('#users-role').setValue('');
    await flushPromises();
    await wrapper.get('#users-status').setValue('banned');
    await flushPromises();
    expect(router.currentRoute.value.query).toEqual({ status: 'banned' });
    expect(shownNames(wrapper)).toEqual(['charlie']);
    await wrapper.findAll('button').find((b) => b.text() === 'Clear')!.trigger('click');
    await flushPromises();
    expect(shownNames(wrapper)).toHaveLength(4);
    wrapper.unmount();

    const linked = await mountTab('/admin/players/accounts?role=admin&q=bra');
    expect((linked.wrapper.get('#users-role').element as HTMLSelectElement).value).toBe('admin');
    expect(shownNames(linked.wrapper)).toEqual(['bravo']);
    linked.wrapper.unmount();
  });

  it('pages the accounts and resets the page when a filter changes', async () => {
    list.mockResolvedValue({ ok: true, value: Array.from({ length: 101 }, (_, i) => userEntry(i + 1)) });
    const { wrapper, router } = await mountTab('/admin/players/accounts?page=2');
    expect(wrapper.findAll('tr.user-row')).toHaveLength(50);
    expect(shownNames(wrapper)[0]).toBe('player51');
    const pages = wrapper.get('nav[aria-label="Account pages"]');
    expect(pages.text()).toContain('Page 2 of 3');
    await pages.findAll('a').find((a) => a.text() === 'Next')!.trigger('click');
    await flushPromises();
    expect(wrapper.findAll('tr.user-row')).toHaveLength(1);
    await wrapper.get('#users-filter').setValue('player1@example.com');
    await flushPromises();
    expect(router.currentRoute.value.query.page).toBeUndefined();
    expect(shownNames(wrapper)).toEqual(['player1']);
    expect(list).toHaveBeenCalledTimes(1);
    wrapper.unmount();
  });

  it('leaves the Online column empty when who is online cannot be read', async () => {
    online.mockResolvedValue({ ok: false, error: { code: 'network', status: null, message: 'Could not reach the server.' } });
    const { wrapper } = await mountTab();
    expect(wrapper.findAll('tr.user-row')).toHaveLength(4);
    expect(wrapper.findAll('.user-online').every((c) => c.text() === '')).toBe(true);
    expect(wrapper.get('[role="alert"]').text()).toBe('');
  });

  it('creates an account from the New account form, and shows a refusal in words', async () => {
    const { wrapper } = await mountTab();
    expect(wrapper.find('#accounts-new').exists()).toBe(false);
    await wrapper.findAll('button').find((b) => b.text() === 'New account')!.trigger('click');
    const form = wrapper.get('#accounts-new');
    await wrapper.get('#accounts-new-username').setValue('echo');
    await wrapper.get('#accounts-new-password').setValue('secret');
    await wrapper.get('#accounts-new-email').setValue('echo@example.com');
    await wrapper.get('#accounts-new-handle').setValue('Echo');
    await wrapper.get('#accounts-new-pcid').setValue('0000BEEF');
    create.mockResolvedValueOnce({ ok: false, error: { code: 'pcid_exists', status: 400, message: 'Another account already has that PCID.' } });
    await form.trigger('submit');
    await flushPromises();
    expect(create).toHaveBeenCalledWith({ username: 'echo', password: 'secret', pcid: '0000BEEF', nwhandle: 'Echo', email: 'echo@example.com' });
    expect(wrapper.get('[role="alert"]').text()).toBe('Another account already has that PCID.');

    // No PCID: the public registration makes one up.
    await wrapper.get('#accounts-new-pcid').setValue('');
    register.mockResolvedValueOnce({ id: 9 } as never);
    await form.trigger('submit');
    await flushPromises();
    expect(register).toHaveBeenCalledWith({ username: 'echo', password: 'secret', nwhandle: 'Echo', email: 'echo@example.com' });
    expect(wrapper.get('[role="status"]').text()).toBe('Created echo.');
    expect(wrapper.find('#accounts-new').exists()).toBe(false);
    expect(list).toHaveBeenCalledTimes(2);
  });
});
