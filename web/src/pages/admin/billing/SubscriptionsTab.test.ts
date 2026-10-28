import * as actionReview from '../../../composables/useConfirm';
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import SubscriptionsTab from './SubscriptionsTab.vue';
import { applyLimitStops, fetchAdminBilling, linkBillingCustomer, reconcileBilling } from '../../../api/adminBilling';
import { fetchTiers } from '../../../api/billing';
import { fetchSettings, saveSettings } from '../../../api/settings';
import { listUsers } from '../../../api/users';
import { adminBilling, subscriptionRow, userEntry } from '../../../test/adminFixtures';
import { tiersResponse } from '../../../test/billingFixtures';
import { serviceSettings } from '../../../test/settingsFixtures';
import { testRouter } from '../../../test/router';

vi.mock('../../../api/adminBilling', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../../api/adminBilling')>()),
  fetchAdminBilling: vi.fn(),
  reconcileBilling: vi.fn(),
  applyLimitStops: vi.fn(),
  linkBillingCustomer: vi.fn(),
}));
vi.mock('../../../api/billing', () => ({ fetchTiers: vi.fn() }));
vi.mock('../../../api/settings', () => ({ fetchSettings: vi.fn(), saveSettings: vi.fn() }));
vi.mock('../../../api/users', () => ({ listUsers: vi.fn() }));

const billing = vi.mocked(fetchAdminBilling);
const reconcile = vi.mocked(reconcileBilling);
const applyStops = vi.mocked(applyLimitStops);
const link = vi.mocked(linkBillingCustomer);
const tiers = vi.mocked(fetchTiers);
const readSettings = vi.mocked(fetchSettings);
const save = vi.mocked(saveSettings);
const users = vi.mocked(listUsers);

const held = {
  limits_hold: true,
  limits_hold_reason: 'a pass would stop 4 server(s) of 3 of 5 owners',
  limits_hold_owners: 3,
  limits_hold_servers: 4,
};

describe('SubscriptionsTab', () => {
  let confirm: MockInstance<typeof actionReview.confirmAction>;
  let wrapper: VueWrapper | null = null;

  async function mountTab() {
    const router = await testRouter('/admin/billing');
    wrapper = mount(SubscriptionsTab, { global: { plugins: [router] } });
    await flushPromises();
    return wrapper;
  }

  function tile(page: VueWrapper, label: string) {
    return page.findAll('.stat-tile').find((t) => t.get('dt').text() === label);
  }

  function cells(row: { findAll: VueWrapper['findAll'] }) {
    return row.findAll('th, td').map((c) => c.text().replace(/\s+/g, ' ').trim());
  }

  beforeEach(() => {
    vi.clearAllMocks();
    confirm = vi.spyOn(actionReview, 'confirmAction').mockResolvedValue(true);
    billing.mockResolvedValue({ ok: true, value: adminBilling() });
    tiers.mockResolvedValue({ ok: true, value: tiersResponse() });
    readSettings.mockResolvedValue({ ok: true, value: serviceSettings({ subscriptions_enabled: true }) });
  });

  afterEach(() => {
    confirm.mockRestore();
    wrapper?.unmount();
    wrapper = null;
  });

  it('is the Billing section: its tabs, the Stripe facts, Reconcile now and no prose', async () => {
    const page = await mountTab();
    expect(page.get('h1').text()).toBe('Billing');
    expect(page.findAll('.tab-nav a').map((a) => a.attributes('href'))).toEqual(['/admin/billing', '/admin/billing/addresses', '/admin/billing/costs']);
    expect(page.get('.page-meta').text()).toContain('Stripe test mode');
    expect(page.get('.page-meta').text()).toContain('Read from Stripe');
    expect(page.findAll('header button').map((b) => b.text())).toContain('Reconcile now');
    expect(page.findAll('p').filter((p) => p.text().length > 120)).toHaveLength(0);
  });

  it('shows subscribers, past due, the monthly revenue and the sold capacity in tiles', async () => {
    const page = await mountTab();
    expect(page.findAll('.stat-tile dt').map((d) => d.text())).toEqual(['Subscribers', 'Past due', 'Revenue a month', 'Shared places sold']);
    expect(tile(page, 'Subscribers')!.text()).toContain('4');
    expect(tile(page, 'Subscribers')!.text()).toContain('1 on tier 1, 2 on tier 2, 1 on tier 3');
    expect(tile(page, 'Past due')!.classes()).toContain('border-warn/40');
    expect(tile(page, 'Revenue a month')!.text()).toContain('about $130');
    expect(tile(page, 'Shared places sold')!.text()).toContain('9 of 12');
    expect(tile(page, 'Shared places sold')!.text()).toContain('2 of 2 dedicated, 0 of 1 addresses');
  });

  it('counts subscribers by tier with a total row at the tiers\' prices', async () => {
    const page = await mountTab();
    // The canceled subscription grants nothing and does not count.
    expect(page.findAll('tr.billing-tier-row').map(cells)).toEqual([
      ['Tier 1', '1', '$5', '$5'],
      ['Tier 2', '2', '$10', '$20'],
      ['Tier 3', '1', '$15', '$15'],
      ['Dedicated machines', '2', '$45', '$90'],
    ]);
    expect(cells(page.get('tr.billing-total'))).toEqual(['Total', '4', '', '$130']);
  });

  it('switches subscriptions on and off sale, saved at once with the fleet as read', async () => {
    save.mockResolvedValue({ ok: true, value: serviceSettings({ subscriptions_enabled: false }) });
    const page = await mountTab();
    const toggle = page.get('button[role="switch"]');
    expect(toggle.attributes('aria-checked')).toBe('true');
    expect(page.get('.billing-on-sale').text()).toContain('On');
    await toggle.trigger('click');
    await flushPromises();
    expect(save).toHaveBeenCalledWith({ subscriptions_enabled: false, max_shared: 2, shared_slots: 12, max_own: 2, max_addresses: 4 });
    expect(page.get('button[role="switch"]').attributes('aria-checked')).toBe('false');
    expect(page.get('[role="status"]').text()).toBe('Subscriptions are off sale.');
  });

  it("shows the switch's refusal and keeps its state", async () => {
    save.mockResolvedValue({ ok: false, error: { code: 'forbidden', status: 403, message: 'This account is not an admin.' } });
    const page = await mountTab();
    await page.get('button[role="switch"]').trigger('click');
    await flushPromises();
    expect(page.get('[role="alert"]').text()).toBe('This account is not an admin.');
    expect(page.get('button[role="switch"]').attributes('aria-checked')).toBe('true');
  });

  it('lists what needs a look in one table with why, each account to its page', async () => {
    billing.mockResolvedValue({
      ok: true,
      value: adminBilling({
        duplicates: [2],
        orphans: [{ stripe_customer_id: 'cus_lost', stripe_subscription_id: 'sub_lost', username_hint: 'echo', status: 'active' }],
      }),
    });
    const page = await mountTab();
    const rows = page.findAll('#billing-look ~ .table-wrap tbody tr');
    expect(rows.map((r) => r.findAll('td')[1].text())).toEqual(['Past due', expect.stringContaining('Canceling'), 'Two live tiers', 'No account']);
    expect(rows[0].get('a').attributes('href')).toBe('/admin/players/3');
    expect(rows[2].get('a').text()).toBe('player2');
    expect(rows[3].text()).toContain('"echo" in Stripe');
  });

  it('shows no Needs a look table when nothing needs one', async () => {
    billing.mockResolvedValue({ ok: true, value: adminBilling({ subscriptions: [subscriptionRow(1)] }) });
    const page = await mountTab();
    expect(page.find('#billing-look').exists()).toBe(false);
  });

  it('lists every subscription with its status badge and Stripe link', async () => {
    billing.mockResolvedValue({
      ok: true,
      value: adminBilling({
        subscriptions: [
          subscriptionRow(9, { status: 'canceled', entitled: false, cancel_at_period_end: true }),
          subscriptionRow(8, { status: 'incomplete', entitled: false, current_period_end: '' }),
        ],
      }),
    });
    const page = await mountTab();
    const [canceled, incomplete] = page.findAll('tr.billing-subscription');
    expect(canceled.get('a').attributes('href')).toBe('/admin/players/9');
    expect(canceled.findAll('.badge').map((b) => b.text())).toEqual(['Canceled']);
    expect(incomplete.findAll('td')[2].text()).toBe('None');
    const stripe = incomplete.findAll('a').at(-1)!;
    expect([stripe.text(), stripe.attributes('href')]).toEqual(['sub_8', 'https://dashboard.stripe.com/test/subscriptions/sub_8']);
  });

  it('reconciles on demand', async () => {
    reconcile.mockResolvedValue({ ok: true, value: undefined });
    const page = await mountTab();
    await page.findAll('button').find((b) => b.text() === 'Reconcile now')!.trigger('click');
    await flushPromises();
    expect(reconcile).toHaveBeenCalledTimes(1);
    expect(page.get('[role="status"]').text()).toBe('Reconcile started.');
  });

  it('relinks an orphan to the account named, once confirmed', async () => {
    billing.mockResolvedValue({
      ok: true,
      value: adminBilling({ orphans: [{ stripe_customer_id: 'cus_lost', stripe_subscription_id: 'sub_lost', username_hint: 'echo', status: 'active' }] }),
    });
    users.mockResolvedValue({ ok: true, value: [userEntry(12, { username: 'Echo' })] });
    link.mockResolvedValue({ ok: true, value: undefined });
    const page = await mountTab();
    const orphan = page.get('tr.billing-orphan');
    await orphan.get('input').setValue('echo');
    await orphan.get('form').trigger('submit');
    await flushPromises();
    expect(confirm).toHaveBeenCalledWith('Link cus_lost to Echo? Its subscriptions count for that account.');
    expect(link).toHaveBeenCalledWith(12, 'cus_lost');
    expect(page.get('[role="status"]').text()).toBe('Linked cus_lost to Echo.');
  });

  it('lists the admin grants, each account to its page, billing on or off', async () => {
    billing.mockResolvedValue({
      ok: true,
      value: adminBilling({
        enabled: false,
        comp_grants: [
          { user_id: 7, username: 'player7', servers: 3, own_machines: 1, addresses: 1 },
          { user_id: 8, username: '', servers: 0, own_machines: 2, addresses: 0 },
        ],
      }),
    });
    const page = await mountTab();
    const grants = page.findAll('tr.billing-grant');
    expect(grants.map((g) => g.get('a').attributes('href'))).toEqual(['/admin/players/7', '/admin/players/8']);
    expect(grants.map(cells)).toEqual([
      ['player7', '3 servers on shared machines, 1 dedicated machine and 1 static address'],
      ['Account 8', '2 dedicated machines'],
    ]);
  });

  it('says when billing is off in one line, and keeps the switch and the capacity', async () => {
    billing.mockResolvedValue({
      ok: true,
      value: adminBilling({
        enabled: false,
        capacity: { shared: { sold: 26, sellable: 24 }, own_machines: { sold: 2, sellable: 2 }, addresses: { sold: 0, sellable: 1 }, oversold: true },
      }),
    });
    const page = await mountTab();
    expect(page.get('.billing-off').text()).toBe('Billing is off: Stripe is not configured.');
    expect(page.findAll('tr.billing-tier-row')).toHaveLength(0);
    expect(page.find('.billing-events').exists()).toBe(false);
    expect(page.find('button[role="switch"]').exists()).toBe(true);
    expect(tile(page, 'Shared places sold')!.text()).toContain('Oversold');
    expect(page.findAll('header button').map((b) => b.text())).not.toContain('Reconcile now');
  });

  it('shows the limits hold in one line and applies its stops once confirmed', async () => {
    billing.mockResolvedValue({ ok: true, value: adminBilling(held) });
    applyStops.mockResolvedValue({ ok: true, value: undefined });
    const page = await mountTab();
    const hold = page.get('.limits-hold');
    expect(hold.text()).toContain('Server limits are on hold: a pass would stop or move 4 servers of 3 owners.');
    expect(hold.get('span').attributes('title')).toBe('a pass would stop 4 server(s) of 3 of 5 owners');
    await hold.get('button').trigger('click');
    await flushPromises();
    expect(confirm).toHaveBeenCalledWith('Stop or move 4 servers of 3 owners now? Their matches end for everyone on them.');
    expect(applyStops).toHaveBeenCalledTimes(1);
    expect(reconcile).not.toHaveBeenCalled();
  });

  it('keeps the hold when the stops are declined', async () => {
    billing.mockResolvedValue({ ok: true, value: adminBilling(held) });
    confirm.mockResolvedValue(false);
    const page = await mountTab();
    await page.get('.limits-hold button').trigger('click');
    await flushPromises();
    expect(applyStops).not.toHaveBeenCalled();
  });

  it('names a sold price missing from the configuration', async () => {
    billing.mockResolvedValue({ ok: true, value: adminBilling({ unknown_prices: ['price_old_tier2'] }) });
    const page = await mountTab();
    expect(page.get('.unknown-prices').text()).toContain('price_old_tier2');
  });

  it('keeps the webhook events behind a disclosure', async () => {
    const page = await mountTab();
    const events = page.get('details.billing-events');
    expect(events.get('summary').text()).toBe('Recent webhook events');
    expect(events.text()).toContain('invoice.paid');
  });
});
