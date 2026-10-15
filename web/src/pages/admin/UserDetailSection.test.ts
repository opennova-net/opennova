import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import UserDetailSection from './UserDetailSection.vue';
import { fetchAccountCdKeys } from '../../api/cdKeys';
import { deleteUser, fetchUser, listUsers, setCompServers, updateUser } from '../../api/users';
import { user, userDetail, userEntry } from '../../test/adminFixtures';
import { hostedServer } from '../../test/hostedFixtures';
import { testRouter } from '../../test/router';

vi.mock('../../api/users', () => ({
  fetchUser: vi.fn(),
  listUsers: vi.fn(),
  updateUser: vi.fn(),
  setCompServers: vi.fn(),
  deleteUser: vi.fn(),
}));
vi.mock('../../api/cdKeys', () => ({ fetchAccountCdKeys: vi.fn() }));

const detail = vi.mocked(fetchUser);
const list = vi.mocked(listUsers);
const update = vi.mocked(updateUser);
const comp = vi.mocked(setCompServers);
const remove = vi.mocked(deleteUser);
const keys = vi.mocked(fetchAccountCdKeys);

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
    const hosting = wrapper.get('#user-hosting').element.parentElement!.textContent!;
    expect(hosting).toContain('2 hosted servers');
    expect(hosting).toContain('Tier 2');
    expect(wrapper.findAll('a').filter((a) => a.attributes('href')?.startsWith('/admin/fleet/'))).toHaveLength(3);
    const customer = wrapper.findAll('a').find((a) => a.text() === 'cus_test_7');
    expect(customer?.attributes('href')).toBe('https://dashboard.stripe.com/test/customers/cus_test_7');
    expect(customer?.attributes('rel')).toContain('noopener');
    expect(wrapper.text()).toContain('No key on this account.');
    expect(wrapper.text()).not.toMatch(/can host|approve/i);
  });

  it('grants comp servers from 0 to 16, confirming a cut below what runs', async () => {
    const { wrapper } = await mountSection();
    comp.mockResolvedValue({ ok: true, value: user(7, { comp_servers: 3, max_servers: 3 }) });
    await wrapper.get('#user-comp-servers').setValue(17);
    expect(wrapper.get('#user-comp-hint').text()).toBe('Enter a whole number from 0 to 16.');
    await wrapper.get('#user-comp-servers').setValue(3);
    await wrapper.get('#user-comp-servers').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(confirm).not.toHaveBeenCalled();
    expect(comp).toHaveBeenCalledWith(7, 3);
    expect(wrapper.get('[role="status"]').text()).toContain('may now run 3 hosted servers free of charge');

    // A tier of 2 servers, 2 live: a grant of 0 stops none; the tier still covers them.
    comp.mockClear();
    await wrapper.get('#user-comp-servers').setValue(0);
    await wrapper.get('#user-comp-servers').element.closest('form')!.dispatchEvent(new Event('submit'));
    await flushPromises();
    expect(confirm).not.toHaveBeenCalled();
    expect(comp).toHaveBeenCalledWith(7, 0);
  });

  it('confirms a cut that stops servers', async () => {
    detail.mockResolvedValue({
      ok: true,
      value: userDetail(7, {
        user: user(7, { comp_servers: 3, max_servers: 3 }),
        entitlement: { max_servers: 3, source: 'comp', tier: null, comp_servers: 3, tier_servers: 0, own_machines: 0, can_start: true },
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

  it("counts the tier's servers under a comp grant, and no stopping server", async () => {
    // Tier 1 (one server) under a grant of 3: cut to 0, the tier's one stays,
    // and of three live servers one is stopping already.
    detail.mockResolvedValue({
      ok: true,
      value: userDetail(7, {
        user: user(7, { comp_servers: 3, max_servers: 3 }),
        entitlement: { max_servers: 3, source: 'comp', tier: 1, comp_servers: 3, tier_servers: 1, own_machines: 0, can_start: true },
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

  it('refuses to delete while a tier grants, in words', async () => {
    remove.mockResolvedValue({
      ok: false,
      error: {
        code: 'active_subscription',
        status: 409,
        message: 'This account has a support tier that still grants hosting. Cancel it in Stripe first; the account can be deleted once it ends.',
      },
    });
    const { wrapper, router } = await mountSection();
    const button = wrapper.findAll('button').find((b) => b.text().startsWith('Delete'))!;
    await button.trigger('click');
    await flushPromises();
    expect(remove).toHaveBeenCalledWith(7);
    expect(wrapper.get('[role="alert"]').text()).toContain('support tier that still grants hosting');
    expect(router.currentRoute.value.path).toBe('/admin/users/7');

    remove.mockResolvedValue({ ok: true, value: undefined });
    await button.trigger('click');
    await flushPromises();
    expect(router.currentRoute.value.path).toBe('/admin/users');
  });
});
