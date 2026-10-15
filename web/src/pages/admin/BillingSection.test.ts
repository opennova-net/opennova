import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi, type MockInstance } from 'vitest';
import BillingSection from './BillingSection.vue';
import { fetchAdminBilling, linkBillingCustomer, reconcileBilling } from '../../api/adminBilling';
import { fetchTiers } from '../../api/billing';
import { listUsers } from '../../api/users';
import { adminBilling, userEntry } from '../../test/adminFixtures';
import { tiersResponse } from '../../test/billingFixtures';
import { testRouter } from '../../test/router';

vi.mock('../../api/adminBilling', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/adminBilling')>()),
  fetchAdminBilling: vi.fn(),
  reconcileBilling: vi.fn(),
  linkBillingCustomer: vi.fn(),
}));
vi.mock('../../api/billing', () => ({ fetchTiers: vi.fn() }));
vi.mock('../../api/users', () => ({ listUsers: vi.fn() }));

const billing = vi.mocked(fetchAdminBilling);
const reconcile = vi.mocked(reconcileBilling);
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
      ['Own machines', '2', '$45', '$90'],
    ]);
    expect(wrapper.get('tfoot').text()).toContain('4');
    expect(wrapper.get('tfoot').text()).toContain('$130');
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

  it('says when billing is off', async () => {
    billing.mockResolvedValue({ ok: true, value: adminBilling({ enabled: false }) });
    const wrapper = await mountSection();
    expect(wrapper.text()).toContain('Billing is off');
    expect(wrapper.findAll('tr.billing-tier-row')).toHaveLength(0);
  });
});
