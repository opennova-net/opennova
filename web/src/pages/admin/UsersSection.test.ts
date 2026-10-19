import { flushPromises, mount } from '@vue/test-utils';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import UsersSection from './UsersSection.vue';
import { createUserAdmin, listUsers, registerUser } from '../../api/users';
import { userEntry } from '../../test/adminFixtures';
import { testRouter } from '../../test/router';
import type { UserEntitlementSummary } from '../../types/users';

vi.mock('../../api/users', () => ({ listUsers: vi.fn(), createUserAdmin: vi.fn(), registerUser: vi.fn() }));

const list = vi.mocked(listUsers);
const create = vi.mocked(createUserAdmin);
const register = vi.mocked(registerUser);

// A users list row's entitlement: none, unless the overrides say otherwise.
function summary(overrides: Partial<UserEntitlementSummary> = {}): UserEntitlementSummary {
  return { max_servers: 0, shared_servers: 0, source: 'none', tier: null, comp_servers: 0, own_machines: 0, comp_own_machines: 0, ...overrides };
}

const USERS = [
  userEntry(1, {
    username: 'alpha',
    max_servers: 2,
    entitlement: summary({ max_servers: 2, shared_servers: 2, source: 'tier', tier: 2 }),
    live_servers: 1,
    subscription_status: 'active',
  }),
  userEntry(2, {
    username: 'bravo',
    comp_servers: 3,
    comp_own_machines: 0,
    max_servers: 3,
    entitlement: summary({ max_servers: 3, shared_servers: 3, source: 'comp', comp_servers: 3 }),
    live_servers: 3,
  }),
  userEntry(3, {
    username: 'charlie',
    entitlement: summary({ max_servers: 1, shared_servers: 1, source: 'tier', tier: 1 }),
    live_servers: 2,
    subscription_status: 'past_due',
    account_status: 'banned',
  }),
  // A grant bigger than the tier still billing beside it: both show.
  userEntry(4, {
    username: 'delta',
    comp_servers: 11,
    max_servers: 11,
    entitlement: summary({ max_servers: 11, shared_servers: 11, source: 'comp', tier: 2, comp_servers: 11 }),
    subscription_status: 'active',
  }),
  // A grant of shared servers and dedicated machines, which add up.
  userEntry(5, {
    username: 'echo',
    comp_servers: 9,
    comp_own_machines: 2,
    max_servers: 11,
    entitlement: summary({ max_servers: 11, shared_servers: 9, source: 'comp', comp_servers: 9, own_machines: 2, comp_own_machines: 2 }),
  }),
  // A dedicated machine paid for, no tier.
  userEntry(6, {
    username: 'foxtrot',
    max_servers: 1,
    entitlement: summary({ max_servers: 1, own_machines: 1 }),
    live_servers: 1,
    subscription_status: null,
  }),
];

async function mountSection() {
  const router = await testRouter('/admin/users');
  const wrapper = mount(UsersSection, { global: { plugins: [router] } });
  await flushPromises();
  return wrapper;
}

describe('UsersSection', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    list.mockResolvedValue({ ok: true, value: USERS });
  });

  it('lists each account with its tier, subscription and servers, each linking to its page', async () => {
    const wrapper = await mountSection();
    const rows = wrapper.findAll('tr.user-row');
    expect(rows).toHaveLength(6);
    expect(rows[0].text()).toContain('Tier 2');
    expect(rows[0].text()).toContain('Active');
    expect(rows[0].text()).toContain('1 live of 2');
    expect(rows[0].get('a').attributes('href')).toBe('/admin/users/1');
    expect(rows[0].text()).toContain('player1@example.com');
    expect(rows[1].text()).toContain('Admin grant of 3 shared');
    expect(rows[1].text()).toContain('None');
    expect(rows[2].text()).toContain('Payment past due');
    expect(rows[2].text()).toContain('Banned');
    // Over its entitlement: marked.
    expect(rows[2].find('.text-danger').text()).toBe('2 live of 1');
    expect(rows[3].text()).toContain('Admin grant of 11 shared, Tier 2');
    expect(rows[4].text()).toContain('Admin grant of 9 shared + 2 dedicated');
    expect(rows[4].text()).toContain('0 live of 11');
    // Dedicated machines alone: a badge of their own, in the tier's colour.
    expect(rows[5].get('td:nth-child(3) span').text()).toBe('1 dedicated');
    expect(rows[5].get('td:nth-child(3) span').classes()).toContain('text-accent');
    expect(rows[4].get('td:nth-child(3) span').classes()).toContain('text-online');
    // The approval toggle is gone.
    expect(wrapper.text()).not.toMatch(/approve|can host/i);
    expect(wrapper.text()).not.toContain('—');
  });

  it('finds an account by name', async () => {
    const wrapper = await mountSection();
    await wrapper.get('#users-filter').setValue('BRA');
    expect(wrapper.findAll('tr.user-row').map((r) => r.get('a').text())).toEqual(['bravo']);
    await wrapper.get('#users-filter').setValue('zulu');
    expect(wrapper.text()).toContain('No account matches.');
  });

  it('creates an account, and shows a refusal in words', async () => {
    const wrapper = await mountSection();
    const form = wrapper.findAll('form').at(-1)!;
    const inputs = form.findAll('input');
    await inputs[0].setValue('delta');
    await inputs[1].setValue('secret');
    await inputs[2].setValue('0000BEEF');
    await inputs[3].setValue('Delta');
    await inputs[4].setValue('delta@example.com');
    create.mockResolvedValueOnce({
      ok: false,
      error: { code: 'pcid_exists', status: 400, message: 'Another account already has that PCID.' },
    });
    await form.trigger('submit');
    await flushPromises();
    expect(create).toHaveBeenCalledWith({
      username: 'delta',
      password: 'secret',
      pcid: '0000BEEF',
      nwhandle: 'Delta',
      email: 'delta@example.com',
    });
    expect(wrapper.get('[role="alert"]').text()).toBe('Another account already has that PCID.');

    // No PCID: the public registration makes one up.
    await inputs[2].setValue('');
    register.mockResolvedValueOnce({ id: 9 } as never);
    await form.trigger('submit');
    await flushPromises();
    expect(register).toHaveBeenCalledWith({ username: 'delta', password: 'secret', nwhandle: 'Delta', email: 'delta@example.com' });
    expect(wrapper.get('[role="status"]').text()).toBe('Created delta.');
  });
});
