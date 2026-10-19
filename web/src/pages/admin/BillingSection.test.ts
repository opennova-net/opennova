import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import BillingSection from './BillingSection.vue';
import { applyLimitStops, fetchAdminBilling, linkBillingCustomer, reconcileBilling } from '../../api/adminBilling';
import { fetchTiers } from '../../api/billing';
import { listUsers } from '../../api/users';
import { adminBilling, subscriptionRow, userEntry } from '../../test/adminFixtures';
import { tiersResponse } from '../../test/billingFixtures';
import { testRouter } from '../../test/router';

vi.mock('../../api/adminBilling', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/adminBilling')>()),
  fetchAdminBilling: vi.fn(),
  reconcileBilling: vi.fn(),
  applyLimitStops: vi.fn(),
  linkBillingCustomer: vi.fn(),
}));
vi.mock('../../api/billing', () => ({ fetchTiers: vi.fn() }));
vi.mock('../../api/users', () => ({ listUsers: vi.fn() }));

const billing = vi.mocked(fetchAdminBilling);
const reconcile = vi.mocked(reconcileBilling);
const applyStops = vi.mocked(applyLimitStops);
const link = vi.mocked(linkBillingCustomer);
const tiers = vi.mocked(fetchTiers);
const users = vi.mocked(listUsers);

describe('BillingSection', () => {
  let confirm: MockInstance<typeof window.confirm>;

  async function mountSection() {
    const router = await testRouter('/admin/billing');
    const wrapper = mount(BillingSection, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  beforeEach(() => {
    vi.clearAllMocks();
    confirm = vi.spyOn(window, 'confirm').mockReturnValue(true);
    billing.mockResolvedValue({ ok: true, value: adminBilling() });
    tiers.mockResolvedValue({ ok: true, value: tiersResponse() });
  });

  afterEach(() => confirm.mockRestore());

  it('counts subscribers by tier and the monthly total at the tiers\' prices', async () => {
    const wrapper = await mountSection();
    const rows = wrapper.findAll('tr.billing-tier-row').map((r) => r.findAll('th, td').map((c) => c.text()));
    // The canceled subscription grants nothing and does not count.
    expect(rows).toEqual([
      ['Tier 1', '1', '$5', '$5'],
      ['Tier 2', '2', '$10', '$20'],
      ['Tier 3', '1', '$15', '$15'],
      ['Dedicated machines', '2', '$45', '$90'],
    ]);
    expect(wrapper.get('tfoot').text()).toContain('4');
    expect(wrapper.get('tfoot').text()).toContain('$130');
  });

  it('counts the static addresses where the deployment sells them', async () => {
    tiers.mockResolvedValue({ ok: true, value: tiersResponse({ static_address: { amount: 500, interval: 'month', available: true } }) });
    billing.mockResolvedValue({
      ok: true,
      value: adminBilling({
        subscriptions: [
          subscriptionRow(1, { tier: 2 }),
          subscriptionRow(1, { item: 'static_address', tier: null, stripe_subscription_id: 'sub_1_address', cancel_at_period_end: true }),
        ],
      }),
    });
    const wrapper = await mountSection();
    const rows = wrapper.findAll('tr.billing-tier-row').map((r) => r.findAll('th, td').map((c) => c.text()));
    expect(rows.at(-1)).toEqual(['Static addresses', '1', '$5', '$5']);
    expect(wrapper.get('tfoot').text()).toContain('$15');
    // Listed as what it is, not as a tier.
    expect(wrapper.text()).toContain('Static address, ends');
    expect(wrapper.text()).not.toContain('Tier ?');
  });

  it('lists the past due and canceling accounts', async () => {
    const wrapper = await mountSection();
    const text = wrapper.text();
    expect(text).toContain('Payments past due (1)');
    expect(text).toContain('Canceling (1)');
    expect(text).toContain('player3');
    expect(text).toContain('Stripe mode');
    expect(text).toContain('Test');
  });

  it('reads a canceled subscription as canceled, not canceling', async () => {
    billing.mockResolvedValue({
      ok: true,
      value: adminBilling({ subscriptions: [subscriptionRow(9, { status: 'canceled', entitled: false, cancel_at_period_end: true })] }),
    });
    const wrapper = await mountSection();
    const row = wrapper.findAll('tr').find((r) => r.text().includes('player9'))!;
    expect(row.text()).toContain('Canceled');
    expect(row.text()).not.toContain('canceling');
    expect(wrapper.text()).toContain('Canceling (0)');
  });

  it("links Stripe's test dashboard in test mode", async () => {
    const wrapper = await mountSection();
    const hrefs = wrapper.findAll('a').map((a) => a.attributes('href') ?? '');
    expect(hrefs).toContain('https://dashboard.stripe.com/test/customers/cus_1');
    expect(hrefs).toContain('https://dashboard.stripe.com/test/subscriptions/sub_1');
  });

  it('reconciles on demand', async () => {
    reconcile.mockResolvedValue({ ok: true, value: undefined });
    const wrapper = await mountSection();
    await wrapper.findAll('button').find((b) => b.text() === 'Reconcile now')!.trigger('click');
    await flushPromises();
    expect(reconcile).toHaveBeenCalledTimes(1);
    expect(wrapper.get('[role="status"]').text()).toContain('Reconcile started');
  });

  it('says None for a subscription with no period end', async () => {
    billing.mockResolvedValue({
      ok: true,
      value: adminBilling({ subscriptions: [subscriptionRow(9, { status: 'incomplete', entitled: false, current_period_end: '' })] }),
    });
    const wrapper = await mountSection();
    const cells = wrapper.findAll('tbody tr').find((r) => r.text().includes('player9'));
    expect(cells?.findAll('td')[2].text()).toBe('None');
  });

  it('relinks an orphan to the account named', async () => {
    billing.mockResolvedValue({
      ok: true,
      value: adminBilling({
        orphans: [{ stripe_customer_id: 'cus_lost', stripe_subscription_id: 'sub_lost', username_hint: 'echo', status: 'active' }],
      }),
    });
    users.mockResolvedValue({ ok: true, value: [userEntry(12, { username: 'Echo' })] });
    link.mockResolvedValue({ ok: true, value: undefined });
    const wrapper = await mountSection();
    const orphan = wrapper.get('li.billing-orphan');
    expect(orphan.text()).toContain('named echo in Stripe');
    await orphan.get('input').setValue('echo');
    await orphan.get('form').trigger('submit');
    await flushPromises();
    expect(link).toHaveBeenCalledWith(12, 'cus_lost');
    expect(wrapper.get('[role="status"]').text()).toBe('Linked cus_lost to Echo.');
  });

  it('shows the sold capacity and the admin-granted accounts, each to its page', async () => {
    billing.mockResolvedValue({
      ok: true,
      value: adminBilling({
        comp_grants: [
          { user_id: 7, username: 'player7', servers: 3, own_machines: 1, addresses: 1 },
          { user_id: 8, username: '', servers: 0, own_machines: 2, addresses: 0 },
        ],
      }),
    });
    const wrapper = await mountSection();
    expect(wrapper.get('section.billing-capacity').text()).toContain(
      'Sold: 9 of 12 shared places, 2 of 2 dedicated machines, 0 of 1 static addresses.',
    );
    expect(wrapper.find('.billing-oversold').exists()).toBe(false);
    const grants = wrapper.findAll('li.billing-grant');
    expect(grants.map((g) => g.get('a').text())).toEqual(['player7', 'Account 8']);
    expect(grants.map((g) => g.get('span').text())).toEqual([
      '3 servers on shared machines, 1 dedicated machine and 1 static address',
      '2 dedicated machines',
    ]);
    expect(grants.map((g) => g.get('a').attributes('href'))).toEqual(['/admin/users/7', '/admin/users/8']);
    expect(wrapper.text()).toContain('Admin grants (2)');
  });

  it('warns while grants and subscriptions oversell the fleet, billing off too', async () => {
    billing.mockResolvedValue({
      ok: true,
      value: adminBilling({
        enabled: false,
        capacity: {
          shared: { sold: 26, sellable: 24 },
          own_machines: { sold: 2, sellable: 2 },
          addresses: { sold: 0, sellable: 1 },
          oversold: true,
        },
        comp_grants: [{ user_id: 7, username: 'player7', servers: 16, own_machines: 0, addresses: 0 }],
      }),
    });
    const wrapper = await mountSection();
    expect(wrapper.text()).toContain('Billing is off');
    expect(wrapper.get('section.billing-capacity').text()).toContain('Sold: 26 of 24 shared places');
    expect(wrapper.get('.billing-oversold').text()).toContain('Admin grants and subscriptions exceed what the fleet sells.');
    expect(wrapper.get('li.billing-grant').text()).toContain('16 servers on shared machines');
  });

  it('says when billing is off', async () => {
    billing.mockResolvedValue({ ok: true, value: adminBilling({ enabled: false }) });
    const wrapper = await mountSection();
    expect(wrapper.text()).toContain('Billing is off');
    expect(wrapper.findAll('tr.billing-tier-row')).toHaveLength(0);
    expect(wrapper.find('section.limits-hold').exists()).toBe(false);
  });

  const held = {
    limits_hold: true,
    limits_hold_reason: 'a pass would stop 4 server(s) of 3 of 5 owners',
    limits_hold_owners: 3,
    limits_hold_servers: 4,
  };

  it('shows the limits hold with its reason, and applies its stops after a second step', async () => {
    billing.mockResolvedValue({ ok: true, value: adminBilling(held) });
    applyStops.mockResolvedValue({ ok: true, value: undefined });
    const wrapper = await mountSection();
    const hold = wrapper.get('section.limits-hold');
    expect(hold.text()).toContain('Hosted server limits are on hold');
    // A pass's stops and its moves off dedicated machines, held alike.
    expect(hold.text()).toContain('A pass would stop or move 4 servers of 3 owners');
    expect(hold.text()).toContain('a pass would stop 4 server(s) of 3 of 5 owners');
    expect(hold.text()).toContain('next full read from Stripe');
    await hold.findAll('button').find((b) => b.text() === 'Apply the stops')!.trigger('click');
    // The first step asks, naming what stops or moves; nothing is sent yet.
    expect(applyStops).not.toHaveBeenCalled();
    expect(wrapper.get('section.limits-hold').text()).toContain('move to a shared machine while there is room, else stop');
    const confirm = wrapper
      .get('section.limits-hold')
      .findAll('button')
      .find((b) => b.text() === 'Stop or move 4 servers of 3 owners');
    expect(confirm).toBeDefined();
    await confirm!.trigger('click');
    await flushPromises();
    expect(applyStops).toHaveBeenCalledTimes(1);
    expect(reconcile).not.toHaveBeenCalled();
    expect(wrapper.get('[role="status"]').text()).toContain("supervisor's next pass");
  });

  it('keeps the hold when the second step is declined', async () => {
    billing.mockResolvedValue({ ok: true, value: adminBilling(held) });
    const wrapper = await mountSection();
    await wrapper.get('section.limits-hold').findAll('button').find((b) => b.text() === 'Apply the stops')!.trigger('click');
    await wrapper.get('section.limits-hold').findAll('button').find((b) => b.text() === 'Keep the hold')!.trigger('click');
    expect(applyStops).not.toHaveBeenCalled();
    expect(wrapper.get('section.limits-hold').text()).toContain('Apply the stops');
  });

  it('names a sold price missing from the configuration, and why the hold waits', async () => {
    billing.mockResolvedValue({ ok: true, value: adminBilling({ ...held, unknown_prices: ['price_old_tier2'] }) });
    const wrapper = await mountSection();
    const missing = wrapper.get('section.unknown-prices');
    expect(missing.text()).toContain('A sold price is missing from the configuration');
    expect(missing.text()).toContain('price_old_tier2');
    expect(wrapper.get('section.limits-hold').text()).toContain('no read from Stripe releases it until that price is listed');
  });

  it('shows no missing-price note when every price is configured', async () => {
    const wrapper = await mountSection();
    expect(wrapper.find('section.unknown-prices').exists()).toBe(false);
  });

  it('shows the limits hold with billing off too', async () => {
    billing.mockResolvedValue({ ok: true, value: adminBilling({ ...held, enabled: false }) });
    const wrapper = await mountSection();
    expect(wrapper.text()).toContain('Billing is off');
    expect(wrapper.get('section.limits-hold').text()).toContain('no read from Stripe can release it');
  });
});
