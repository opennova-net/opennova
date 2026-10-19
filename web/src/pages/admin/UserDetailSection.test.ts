import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import UserDetailSection from './UserDetailSection.vue';
import { addCompAddress } from '../../api/adminAddresses';
import { fetchAccountCdKeys } from '../../api/cdKeys';
import { deleteUser, fetchUser, listUsers, setCompGrants, updateUser } from '../../api/users';
import { adminAddress } from '../../test/addressFixtures';
import { user, userDetail, userEntry } from '../../test/adminFixtures';
import { hostedServer } from '../../test/hostedFixtures';
import { testRouter } from '../../test/router';
import type { SoldCapacity } from '../../types/users';

vi.mock('../../api/users', () => ({
  fetchUser: vi.fn(),
  listUsers: vi.fn(),
  updateUser: vi.fn(),
  setCompGrants: vi.fn(),
  deleteUser: vi.fn(),
}));
vi.mock('../../api/cdKeys', () => ({ fetchAccountCdKeys: vi.fn() }));
vi.mock('../../api/adminAddresses', () => ({ addCompAddress: vi.fn() }));

const detail = vi.mocked(fetchUser);
const list = vi.mocked(listUsers);
const update = vi.mocked(updateUser);
const comp = vi.mocked(setCompGrants);
const remove = vi.mocked(deleteUser);
const keys = vi.mocked(fetchAccountCdKeys);
const addAddress = vi.mocked(addCompAddress);

// Shared places sold past the fleet: an admin's grant took them there.
const OVERSOLD: SoldCapacity = {
  shared: { sold: 26, sellable: 24 },
  own_machines: { sold: 4, sellable: 4 },
  addresses: { sold: 1, sellable: 4 },
  oversold: true,
};

async function mountSection() {
  const router = await testRouter('/admin/users/7', ['/admin/users/:id']);
  const wrapper = mount(UserDetailSection, { global: { plugins: [router] } });
  await flushPromises();
  return { wrapper, router };
}

describe('UserDetailSection', () => {
  let confirm: MockInstance<typeof window.confirm>;

  beforeEach(() => {
    vi.clearAllMocks();
    confirm = vi.spyOn(window, 'confirm').mockReturnValue(true);
    detail.mockResolvedValue({
      ok: true,
      value: userDetail(7, {
        servers: [hostedServer(31), hostedServer(32), hostedServer(30, { state: 'stopped' })],
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
      }),
    });
    keys.mockResolvedValue({ ok: true, value: { account: { id: 7, username: 'player7' }, keys: [], activations: [] } });
  });

  afterEach(() => confirm.mockRestore());

  it("shows the profile, the entitlement, the servers and Stripe's ids", async () => {
    const { wrapper } = await mountSection();
    expect(detail).toHaveBeenCalledWith(7);
    expect(wrapper.get('h1').text()).toBe('player7');
    // The email, read-only: no field edits it.
    expect(wrapper.get('header').text()).toContain('player7@example.com');
    expect(wrapper.findAll('input').some((i) => (i.element as HTMLInputElement).value === 'player7@example.com')).toBe(false);
    const hosting = wrapper.get('#user-hosting').element.parentElement!.textContent!;
    expect(hosting).toContain('2 hosted servers');
    expect(hosting).toContain('Tier 2');
    expect(hosting).toContain('All on shared machines');
    expect(wrapper.findAll('a').filter((a) => a.attributes('href')?.startsWith('/admin/fleet/'))).toHaveLength(3);
    const customer = wrapper.findAll('a').find((a) => a.text() === 'cus_test_7');
    expect(customer?.attributes('href')).toBe('https://dashboard.stripe.com/test/customers/cus_test_7');
    expect(customer?.attributes('rel')).toContain('noopener');
    expect(wrapper.text()).toContain('No key on this account.');
    expect(wrapper.text()).not.toMatch(/can host|approve/i);
  });

  it("says a dedicated machine moves one of the grant's servers, adding none", async () => {
    const base = userDetail(7);
    detail.mockResolvedValue({
      ok: true,
      value: userDetail(7, {
        entitlement: { ...base.entitlement, max_servers: 11, source: 'comp', comp_servers: 11, comp_own_machines: 2, own_machines: 2 },
      }),
    });
    const { wrapper } = await mountSection();
    const hosting = wrapper.get('#user-hosting').element.parentElement!.textContent!;
    expect(hosting).toContain('11 hosted servers');
    expect(hosting).toContain('9 on shared machines, 2 on dedicated machines');
    expect(wrapper.get('#user-comp-hint').text()).toContain('Dedicated machines add no servers');
  });

  it('grants comp servers and dedicated machines from 0 to 16, confirming a cut below what runs', async () => {
    const { wrapper } = await mountSection();
    comp.mockResolvedValue({
      ok: true,
      value: { user: user(7, { comp_servers: 3, comp_own_machines: 1, max_servers: 3 }), capacity: null },
    });
    await wrapper.get('#user-comp-servers').setValue(17);
    expect(wrapper.get('#user-comp-hint').text()).toBe('Enter a whole number from 0 to 16.');
    await wrapper.get('#user-comp-servers').setValue(3);
    await wrapper.get('#user-comp-own').setValue(-1);
    expect(wrapper.get('#user-comp-hint').text()).toBe('Enter a whole number from 0 to 16.');
    await wrapper.get('#user-comp-own').setValue(1);
    await wrapper.get('#user-comp-servers').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(confirm).not.toHaveBeenCalled();
    expect(comp).toHaveBeenCalledWith(7, { servers: 3, own_machines: 1 });
    expect(wrapper.get('[role="status"]').text()).toContain("grant is now 3 hosted servers and 1 dedicated machine for its servers, free of charge");

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
    const { wrapper } = await mountSection();
    expect(wrapper.find('#user-oversold').exists()).toBe(false);

    // The grant's reply carries the sold capacity: the warning shows from it,
    // even before (or without) the page's next read.
    comp.mockResolvedValue({ ok: true, value: { user: user(7, { comp_servers: 6, max_servers: 6 }), capacity: OVERSOLD } });
    detail.mockResolvedValueOnce({ ok: false, error: { code: 'network', status: null, message: 'Could not reach the server.' } });
    await wrapper.get('#user-comp-servers').setValue(6);
    await wrapper.get('#user-comp-servers').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(wrapper.get('#user-oversold').text()).toBe(
      'Admin grants and subscriptions exceed what the fleet sells: shared places 26 of 24, dedicated machines 4 of 4, ' +
        'addresses 1 of 4. New subscriptions of those items are paused.',
    );

    // And on the page's own read.
    detail.mockResolvedValue({ ok: true, value: userDetail(7, { capacity: OVERSOLD }) });
    const again = await mountSection();
    expect(again.wrapper.get('#user-oversold').text()).toContain('shared places 26 of 24');
  });

  it("lists the account's addresses and adds a comp one, checking its name first", async () => {
    detail.mockResolvedValue({
      ok: true,
      value: userDetail(7, {
        addresses: [
          adminAddress(1, 'smoke', { server: { id: 31, game_name: 'Night Ops', state: 'running' } }),
          adminAddress(2, 'spare', { source: 'comp', stripe_subscription_id: null, state: 'allocating', public_ip: null, port: null }),
        ],
      }),
    });
    const { wrapper } = await mountSection();
    const list = wrapper.get('#user-addresses').element.parentElement!.textContent!;
    expect(list).toContain('smoke.play.opennova.net');
    expect(list).toContain('203.0.113.1:32768, its own subscription');
    expect(list).toContain('No IP yet, an admin grant');
    expect(list).toContain('Being set up');
    expect(wrapper.findAll('a').find((a) => a.text() === 'Night Ops')?.attributes('href')).toBe('/admin/fleet/31');

    await wrapper.get('#user-comp-address').setValue('Bad--Name');
    await wrapper.get('#user-comp-address').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(addAddress).not.toHaveBeenCalled();
    expect(wrapper.get('#user-comp-address-hint').text()).toContain('3 to 32 lower-case letters');
    expect(wrapper.get('#user-comp-address').attributes('aria-invalid')).toBe('true');

    addAddress.mockResolvedValue({ ok: true, value: adminAddress(3, 'arena', { state: 'allocating' }) });
    await wrapper.get('#user-comp-address').setValue('arena');
    await wrapper.get('#user-comp-address').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(addAddress).toHaveBeenCalledWith(7, 'arena');
    expect(wrapper.get('[role="status"]').text()).toBe('player7 has the comp address arena.play.opennova.net; a worker sets it up now.');
  });

  it('takes the comp address form away when the service sells no address', async () => {
    addAddress.mockResolvedValue({
      ok: false,
      error: { code: 'addresses_disabled', status: 503, message: 'This NovaWorld service sells no static address.' },
    });
    const { wrapper } = await mountSection();
    await wrapper.get('#user-comp-address').setValue('arena');
    await wrapper.get('#user-comp-address').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(wrapper.get('[role="alert"]').text()).toContain('sells no static address');
    expect(wrapper.find('#user-comp-address').exists()).toBe(false);
  });

  it('confirms a cut that stops servers', async () => {
    detail.mockResolvedValue({
      ok: true,
      value: userDetail(7, {
        user: user(7, { comp_servers: 3, max_servers: 3 }),
        entitlement: { max_servers: 3, source: 'comp', tier: null, comp_servers: 3, comp_own_machines: 0, tier_servers: 0, own_machines: 0, can_start: true },
        servers: [hostedServer(31), hostedServer(32), hostedServer(33)],
      }),
    });
    const { wrapper } = await mountSection();
    confirm.mockReturnValueOnce(false);
    await wrapper.get('#user-comp-servers').setValue(1);
    await wrapper.get('#user-comp-servers').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(confirm).toHaveBeenCalledWith(expect.stringContaining('stops its 2 newest servers'));
    expect(comp).not.toHaveBeenCalled();
  });

  it('confirms a cut of dedicated machines below the servers on them: they move while there is room', async () => {
    // A grant of 3 servers on 2 dedicated machines, both in use.
    detail.mockResolvedValue({
      ok: true,
      value: userDetail(7, {
        user: user(7, { comp_servers: 3, comp_own_machines: 2, max_servers: 3 }),
        entitlement: { max_servers: 3, source: 'comp', tier: null, comp_servers: 3, comp_own_machines: 2, tier_servers: 0, own_machines: 2, can_start: true },
        servers: [hostedServer(31, { own_machine: true }), hostedServer(32, { own_machine: true }), hostedServer(33)],
      }),
    });
    const { wrapper } = await mountSection();
    confirm.mockReturnValueOnce(false);
    await wrapper.get('#user-comp-own').setValue(0);
    await wrapper.get('#user-comp-own').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(confirm).toHaveBeenCalledWith(
      "Lower player7's dedicated machines to 0? 2 of their servers on dedicated machines move to a shared machine (their matches restart) while there is room; the rest stop.",
    );
    expect(comp).not.toHaveBeenCalled();

    // Down to one: one of the two moves. Down to two: nothing to confirm.
    confirm.mockClear();
    confirm.mockReturnValueOnce(false);
    await wrapper.get('#user-comp-own').setValue(1);
    await wrapper.get('#user-comp-own').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(confirm).toHaveBeenCalledWith(expect.stringContaining("Lower player7's dedicated machines to 1? One of their servers on dedicated machines moves"));
    comp.mockResolvedValue({ ok: true, value: { user: user(7, { comp_servers: 2, comp_own_machines: 2, max_servers: 2 }), capacity: null } });
    confirm.mockClear();
    await wrapper.get('#user-comp-own').setValue(2);
    await wrapper.get('#user-comp-servers').setValue(2);
    await wrapper.get('#user-comp-own').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    // The server cut stops the newest (a shared one): both own-machine servers keep their machines.
    expect(confirm).toHaveBeenCalledWith(expect.stringContaining('stops its newest server'));
    expect(confirm).not.toHaveBeenCalledWith(expect.stringContaining('dedicated machines to'));
    expect(comp).toHaveBeenCalledWith(7, { servers: 2, own_machines: 2 });
  });

  it("counts the add-on's dedicated machines beside the grant's", async () => {
    // Tier 2 with one dedicated machine paid for, and an admin's one more: two in use.
    detail.mockResolvedValue({
      ok: true,
      value: userDetail(7, {
        user: user(7, { comp_servers: 0, comp_own_machines: 1, max_servers: 2 }),
        entitlement: { max_servers: 2, source: 'tier', tier: 2, comp_servers: 0, comp_own_machines: 1, tier_servers: 2, own_machines: 2, can_start: true },
        servers: [hostedServer(31, { own_machine: true }), hostedServer(32, { own_machine: true })],
      }),
    });
    const { wrapper } = await mountSection();
    confirm.mockReturnValueOnce(false);
    await wrapper.get('#user-comp-own').setValue(0);
    await wrapper.get('#user-comp-own').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(confirm).toHaveBeenCalledWith(expect.stringContaining("Lower player7's dedicated machines to 1? One of their servers"));
    expect(comp).not.toHaveBeenCalled();
  });

  it("counts the tier's servers under a comp grant, and no stopping server", async () => {
    // Tier 1 (one server) under a grant of 3: cut to 0, the tier's one stays,
    // and of three live servers one is stopping already.
    detail.mockResolvedValue({
      ok: true,
      value: userDetail(7, {
        user: user(7, { comp_servers: 3, max_servers: 3 }),
        entitlement: { max_servers: 3, source: 'comp', tier: 1, comp_servers: 3, comp_own_machines: 0, tier_servers: 1, own_machines: 0, can_start: true },
        servers: [hostedServer(31), hostedServer(32), hostedServer(33, { state: 'stopping' })],
      }),
    });
    const { wrapper } = await mountSection();
    confirm.mockReturnValueOnce(false);
    await wrapper.get('#user-comp-servers').setValue(0);
    await wrapper.get('#user-comp-servers').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(confirm).toHaveBeenCalledWith(expect.stringContaining('They run 2 servers live, so the service stops its newest server'));
    expect(comp).not.toHaveBeenCalled();
  });

  it('changes the role after a confirmation judged on every admin', async () => {
    list.mockResolvedValue({ ok: true, value: [userEntry(7), userEntry(8, { role: 'admin' })] });
    update.mockResolvedValue({ ok: true, value: user(7, { role: 'admin' }) });
    const { wrapper } = await mountSection();
    const button = wrapper.findAll('button').find((b) => b.text() === 'Make admin')!;
    await button.trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenCalledWith(expect.stringContaining('Make player7 an admin?'));
    expect(update).toHaveBeenCalledWith(7, { role: 'admin' });
    expect(wrapper.get('[role="status"]').text()).toBe('player7 is now an admin.');
  });

  it('refuses to delete while a subscription grants, in words', async () => {
    remove.mockResolvedValue({
      ok: false,
      error: {
        code: 'active_subscription',
        status: 409,
        message:
          'This account has a subscription that still grants (a support tier or an add-on). Cancel it in Stripe first; the account can be deleted once its subscriptions end.',
      },
    });
    const { wrapper, router } = await mountSection();
    const button = wrapper.findAll('button').find((b) => b.text().startsWith('Delete'))!;
    await button.trigger('click');
    await flushPromises();
    expect(remove).toHaveBeenCalledWith(7);
    expect(wrapper.text()).toContain('cannot be deleted until its subscriptions end, nor while a checkout of it is open');
    expect(wrapper.get('[role="alert"]').text()).toContain('a subscription that still grants');
    expect(router.currentRoute.value.path).toBe('/admin/users/7');

    remove.mockResolvedValue({ ok: true, value: undefined });
    await button.trigger('click');
    await flushPromises();
    expect(router.currentRoute.value.path).toBe('/admin/users');
  });
});
