import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createMemoryHistory, createRouter, type Router } from 'vue-router';
import BillingPage from './BillingPage.vue';
import { fetchMyAddresses, nameAddress } from '../../api/addresses';
import { cancelCheckout, fetchMyBilling, fetchTiers, openPortal, startCheckout, syncMyBilling } from '../../api/billing';
import { address, addressesResponse } from '../../test/addressFixtures';
import {
  addressSubscription,
  myBilling,
  ownMachineSubscription,
  subscribed,
  subscription,
  tiersResponse,
} from '../../test/billingFixtures';
import { navigateToStripe } from '../../utils/navigate';

vi.mock('../../api/billing', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/billing')>()),
  cancelCheckout: vi.fn(),
  fetchTiers: vi.fn(),
  fetchMyBilling: vi.fn(),
  syncMyBilling: vi.fn(),
  startCheckout: vi.fn(),
  openPortal: vi.fn(),
}));
vi.mock('../../api/addresses', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/addresses')>()),
  fetchMyAddresses: vi.fn(),
  nameAddress: vi.fn(),
}));
vi.mock('../../utils/navigate', () => ({ navigateToStripe: vi.fn(() => true) }));

const tiers = vi.mocked(fetchTiers);
const mine = vi.mocked(fetchMyBilling);
const sync = vi.mocked(syncMyBilling);
const checkout = vi.mocked(startCheckout);
const portal = vi.mocked(openPortal);
const cancel = vi.mocked(cancelCheckout);
const navigate = vi.mocked(navigateToStripe);
const addresses = vi.mocked(fetchMyAddresses);
const name = vi.mocked(nameAddress);

const CHECKOUT_URL = 'https://checkout.stripe.com/c/pay/cs_test_1';
const PORTAL_URL = 'https://billing.stripe.com/p/session/test_1';

let router: Router;
let wrapper: ReturnType<typeof mount> | null = null;

async function mountAt(path: string) {
  router = createRouter({
    history: createMemoryHistory(),
    routes: [{ path: '/:rest(.*)*', component: { render: () => null } }],
  });
  await router.push(path);
  wrapper = mount(BillingPage, { global: { plugins: [router] } });
  await flushPromises();
  return wrapper;
}

function statusText(): string {
  return wrapper!.get('[role="status"]').text();
}

describe('BillingPage', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    window.sessionStorage.clear(); // the checkout item a test's checkout remembered
    tiers.mockResolvedValue({ ok: true, value: tiersResponse() });
    mine.mockResolvedValue({ ok: true, value: myBilling() });
    cancel.mockResolvedValue({ ok: true, value: true });
    addresses.mockResolvedValue({ ok: true, value: addressesResponse([]) });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    vi.useRealTimers();
  });

  it('subscribes through a Stripe Checkout', async () => {
    checkout.mockResolvedValue({ ok: true, value: CHECKOUT_URL });
    const page = await mountAt('/me/billing');
    expect(page.findAll('article[aria-current]')).toHaveLength(0);
    await page.get('#billing-tier-2 button').trigger('click');
    expect(checkout).not.toHaveBeenCalled();
    await page.get('#billing-review button').trigger('click');
    await flushPromises();
    expect(checkout).toHaveBeenCalledWith('tier2');
    expect(navigate).toHaveBeenCalledWith(CHECKOUT_URL);
  });

  it('preserves a public tier selection and shows its current price before explicit checkout', async () => {
    checkout.mockResolvedValue({ ok: true, value: CHECKOUT_URL });
    const page = await mountAt('/me/billing?tier=3');
    expect(page.get('#billing-review h3').text()).toBe('Tier 3: $15 a month');
    expect(page.get('#billing-tier-3 button').text()).toContain('Selected');
    expect(checkout).not.toHaveBeenCalled();
    expect(portal).not.toHaveBeenCalled();
    await page.get('#billing-review button').trigger('click');
    await flushPromises();
    expect(checkout).toHaveBeenCalledWith('tier3');
  });

  it('does not silently substitute or buy an unavailable selected tier', async () => {
    const offers = tiersResponse();
    offers.tiers[1].available = false;
    tiers.mockResolvedValue({ ok: true, value: offers });
    const page = await mountAt('/me/billing?tier=2');
    expect(page.get('#billing-review').text()).toContain('Hosting is full');
    expect(page.get('#billing-review button').attributes('disabled')).toBeDefined();
    expect(checkout).not.toHaveBeenCalled();
  });

  it('ignores an invalid tier query instead of selecting or buying anything', async () => {
    const page = await mountAt('/me/billing?tier=999');
    expect(page.find('#billing-review').exists()).toBe(false);
    expect(checkout).not.toHaveBeenCalled();
  });

  it("marks the account's tier and sends every change to the billing portal", async () => {
    mine.mockResolvedValue({ ok: true, value: subscribed(2, { used: 2, shared_used: 2 }) });
    portal.mockResolvedValue({ ok: true, value: PORTAL_URL });
    const page = await mountAt('/me/billing');
    expect(page.get('#billing-tier-2').attributes('aria-current')).toBe('true');
    expect(page.get('#billing-tier-1').attributes('aria-current')).toBeUndefined();
    const facts = page.findAll('#billing-tier > *').map((f) => f.text());
    expect(facts[1]).toBe('Tier 2, up to 2 servers');
    expect(facts.slice(-2)).toEqual(['Live on shared machines', '2 of 2']);
    // Moving down from 2 live servers to Tier 1 stops the newest.
    expect(page.get('#billing-tier-1').text()).toContain(
      'Stops your newest server on a shared machine.',
    );
    await page.get('#billing-tier-3 button').trigger('click');
    await flushPromises();
    expect(checkout).not.toHaveBeenCalled();
    expect(portal).toHaveBeenCalledTimes(1);
    expect(navigate).toHaveBeenCalledWith(PORTAL_URL);
  });

  it('counts no stopping server in what a move down would stop', async () => {
    // Two live on Tier 2, one of them stopping already: Tier 1 keeps the other.
    mine.mockResolvedValue({ ok: true, value: subscribed(2, { used: 2, stopping: 1, shared_used: 2, shared_stopping: 1 }) });
    const page = await mountAt('/me/billing');
    expect(page.get('#billing-tier-1').text()).not.toContain('will be stopped');
  });

  it('counts no server on a dedicated machine in what a move down would stop', async () => {
    // Tier 2 with one server on a shared machine and one on a dedicated machine.
    mine.mockResolvedValue({
      ok: true,
      value: subscribed(2, { max_servers: 3, own_machines: 1, used: 2, shared_used: 1, own_machine_subscriptions: [ownMachineSubscription()] }),
    });
    const page = await mountAt('/me/billing');
    expect(page.get('#billing-tier-1').text()).not.toContain('will be stopped');
  });

  it('opens the portal when the checkout finds a tier already live', async () => {
    checkout.mockResolvedValue({
      ok: false,
      error: { code: 'already_subscribed', status: 409, message: 'You already have a support tier.' },
    });
    portal.mockResolvedValue({ ok: true, value: PORTAL_URL });
    const page = await mountAt('/me/billing');
    await page.get('#billing-tier-1 button').trigger('click');
    await page.get('#billing-review button').trigger('click');
    await flushPromises();
    expect(portal).toHaveBeenCalledTimes(1);
    expect(navigate).toHaveBeenCalledWith(PORTAL_URL);
  });

  it('says what is wrong when Stripe\'s address looks wrong', async () => {
    checkout.mockResolvedValue({ ok: true, value: 'https://evil.example/' });
    navigate.mockReturnValueOnce(false);
    const page = await mountAt('/me/billing');
    await page.get('#billing-tier-1 button').trigger('click');
    await page.get('#billing-review button').trigger('click');
    await flushPromises();
    expect(page.get('[role="alert"]').text()).toContain('looked wrong');
  });

  const ownMachine = ownMachineSubscription();

  it('sells a dedicated machine with no support tier', async () => {
    checkout.mockResolvedValue({ ok: true, value: CHECKOUT_URL });
    const page = await mountAt('/me/billing');
    const section = page.get('#billing-own-machine').element.closest('section')!;
    expect(page.get('#billing-own-machine').text()).toBe('Dedicated machines');
    expect(section.textContent).toContain('$45 a month each');
    const add = page.findAll('button').find((b) => b.text() === 'Add a dedicated machine')!;
    expect(add.attributes('disabled')).toBeUndefined();
    await add.trigger('click');
    await flushPromises();
    expect(checkout).toHaveBeenCalledWith('own_machine');
    expect(navigate).toHaveBeenCalledWith(CHECKOUT_URL);
  });

  it('lists the dedicated machine subscriptions and adds another', async () => {
    // No tier: two dedicated machines, the newer one canceled.
    mine.mockResolvedValue({
      ok: true,
      value: myBilling({
        max_servers: 2,
        own_machines: 2,
        can_start: true,
        can_manage: true,
        own_machine_subscriptions: [
          ownMachineSubscription({ stripe_subscription_id: 'sub_own_2', cancel_at_period_end: true, current_period_end: '2026-11-20T12:00:00Z' }),
          ownMachine,
        ],
      }),
    });
    checkout.mockResolvedValue({ ok: true, value: CHECKOUT_URL });
    const page = await mountAt('/me/billing');
    const section = page.get('#billing-own-machine').element.closest('section')!;
    expect(section).toBeDefined();
    const rows = page.findAll('#billing-own-machine-subscriptions tbody tr').map((tr) => tr.findAll('td').map((td) => td.text().replace(/\s+/g, ' ')));
    expect(rows).toHaveLength(2);
    expect(rows[0][0]).toBe('1');
    expect(rows[0][1]).toBe('Active');
    expect(rows[0][2]).toMatch(/^Ends .*2026$/);
    expect(rows[1][2]).toMatch(/^[A-Z][a-z]+ \d+, 2026$/);
    expect(page.find('#billing-own-machine-block').exists()).toBe(false);
    expect(page.findAll('button').some((b) => b.text() === 'Cancel in Stripe')).toBe(true);
    const add = page.findAll('button').find((b) => b.text() === 'Add another');
    expect(add).toBeDefined();
    await add!.trigger('click');
    await flushPromises();
    expect(checkout).toHaveBeenCalledWith('own_machine');
    // The count it had goes along, for the return to wait for one more.
    expect(JSON.parse(window.sessionStorage.getItem('opennova_checkout')!)).toEqual({
      item: 'own_machine',
      own_machines_before: 2,
      addresses_before: 0,
    });
  });

  it('pauses what the fleet is sold out of, keeping the rest', async () => {
    const offer = tiersResponse();
    tiers.mockResolvedValue({
      ok: true,
      value: tiersResponse({
        tiers: offer.tiers.map((t) => ({ ...t, available: t.tier !== 3 })),
        own_machine: { amount: 4500, interval: 'month', available: false },
      }),
    });
    mine.mockResolvedValue({ ok: true, value: subscribed(3, { own_machines: 1, own_machine_subscriptions: [ownMachine] }) });
    const page = await mountAt('/me/billing');
    // The account's own tier keeps its Manage while the fleet is full.
    expect(page.get('#billing-tier-3 button').attributes('disabled')).toBeUndefined();
    expect(page.get('#billing-tier-3').text()).not.toContain('Hosting is full');
    const add = page.findAll('button').find((b) => b.text() === 'Add another')!;
    expect(add.attributes('disabled')).toBeDefined();
    expect(page.get('#billing-own-machine-block').text()).toBe('Hosting is full: new subscriptions are paused.');
    expect(page.findAll('button').some((b) => b.text() === 'Cancel in Stripe' && b.attributes('disabled') === undefined)).toBe(true);
  });

  it('reads the tiers again when a checkout finds the fleet sold out', async () => {
    checkout.mockResolvedValue({
      ok: false,
      error: { code: 'sold_out', status: 409, message: 'Hosting is full: new subscriptions are paused. Try again later; no payment was taken.' },
    });
    const page = await mountAt('/me/billing');
    const offer = tiersResponse();
    tiers.mockResolvedValue({ ok: true, value: tiersResponse({ tiers: offer.tiers.map((t) => ({ ...t, available: false })) }) });
    await page.get('#billing-tier-1 button').trigger('click');
    await page.get('#billing-review button').trigger('click');
    await flushPromises();
    expect(page.get('[role="alert"]').text()).toContain('no payment was taken');
    expect(tiers).toHaveBeenCalledTimes(2);
    expect(page.get('#billing-tier-1').text()).toContain('Hosting is full: new subscriptions are paused.');
    expect(page.get('#billing-tier-1 button').attributes('disabled')).toBeDefined();
    expect(navigate).not.toHaveBeenCalled();
  });

  const STATIC_ADDRESS = { amount: 500, interval: 'month', available: true };

  it('sells a static address for each dedicated machine in its addresses section', async () => {
    tiers.mockResolvedValue({ ok: true, value: tiersResponse({ static_address: STATIC_ADDRESS }) });
    mine.mockResolvedValue({
      ok: true,
      value: subscribed(3, {
        own_machines: 2,
        own_machine_subscriptions: [ownMachineSubscription({ quantity: 2 })],
        static_addresses: 1,
        address_subscriptions: [addressSubscription()],
      }),
    });
    checkout.mockResolvedValue({ ok: true, value: CHECKOUT_URL });
    const page = await mountAt('/me/billing');
    const section = page.get('section#addresses');
    expect(section.get('h2').text()).toBe('Static addresses');
    expect(section.text()).toContain('$5 a month each');
    const add = page.findAll('button').find((b) => b.text() === 'Add a static address')!;
    expect(add.attributes('disabled')).toBeUndefined();
    await add.trigger('click');
    await flushPromises();
    expect(checkout).toHaveBeenCalledWith('static_address');
    expect(JSON.parse(window.sessionStorage.getItem('opennova_checkout')!)).toMatchObject({
      item: 'static_address',
      addresses_before: 1,
    });
    expect(navigate).toHaveBeenCalledWith(CHECKOUT_URL);
  });

  it('says why a static address cannot be bought now', async () => {
    const offered = tiersResponse({ static_address: STATIC_ADDRESS });
    tiers.mockResolvedValue({ ok: true, value: offered });
    // No dedicated machine.
    mine.mockResolvedValue({ ok: true, value: subscribed(2) });
    let page = await mountAt('/me/billing');
    const button = () => page.findAll('button').find((b) => b.text() === 'Add a static address')!;
    expect(button().attributes('disabled')).toBeDefined();
    expect(page.get('#addresses-add-block').text()).toBe('Needs a dedicated machine first.');
    page.unmount();

    // One dedicated machine, already with its address.
    mine.mockResolvedValue({
      ok: true,
      value: subscribed(2, { own_machines: 1, own_machine_subscriptions: [ownMachine], static_addresses: 1, address_subscriptions: [addressSubscription()] }),
    });
    page = await mountAt('/me/billing');
    expect(button().attributes('disabled')).toBeDefined();
    expect(page.get('#addresses-add-block').text()).toBe('Each dedicated machine has one already.');
    page.unmount();

    // Room for one, and the fleet sold out of addresses.
    tiers.mockResolvedValue({ ok: true, value: tiersResponse({ static_address: { ...STATIC_ADDRESS, available: false } }) });
    mine.mockResolvedValue({ ok: true, value: subscribed(2, { own_machines: 1, own_machine_subscriptions: [ownMachine] }) });
    page = await mountAt('/me/billing');
    expect(button().attributes('disabled')).toBeDefined();
    expect(page.get('#addresses-add-block').text()).toBe('Hosting is full: new subscriptions are paused.');
  });

  it('says what an admin granted and still offers everything to buy beside it', async () => {
    tiers.mockResolvedValue({ ok: true, value: tiersResponse({ static_address: STATIC_ADDRESS }) });
    mine.mockResolvedValue({
      ok: true,
      value: myBilling({
        max_servers: 11,
        shared_servers: 9,
        comp_servers: 9,
        comp_own_machines: 2,
        comp_addresses: 1,
        granted: true,
        own_machines: 2,
        can_start: true,
        can_subscribe: true,
      }),
    });
    const page = await mountAt('/me/billing');
    expect(page.get('#billing-granted').text()).toBe('9 shared servers, 2 dedicated machines and 1 static address');
    for (const id of ['#billing-tiers', '#billing-own-machine', '#addresses']) expect(page.find(id).exists()).toBe(true);
    expect(page.find('#billing-off-sale').exists()).toBe(false);
  });

  it('sells nothing while the admin has subscriptions off, and says so', async () => {
    tiers.mockResolvedValue({ ok: true, value: tiersResponse({ on_sale: false, static_address: STATIC_ADDRESS }) });
    const page = await mountAt('/me/billing');
    expect(page.get('#billing-off-sale').text()).toBe('Nothing is on sale right now.');
    for (const id of ['#billing-tiers', '#billing-own-machine', '#addresses']) expect(page.find(id).exists()).toBe(false);
    expect(page.findAll('button').some((b) => /checkout|dedicated machine|static address/i.test(b.text()))).toBe(false);
  });

  it('shows no addresses section where none is sold and the account has none', async () => {
    const page = await mountAt('/me/billing');
    expect(page.find('#addresses').exists()).toBe(false);
  });

  it('confirms a static address once one more of them is active', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout', 'Date'] });
    tiers.mockResolvedValue({ ok: true, value: tiersResponse({ static_address: STATIC_ADDRESS }) });
    window.sessionStorage.setItem('opennova_checkout', JSON.stringify({ item: 'static_address', addresses_before: 0 }));
    const before = subscribed(2, { own_machines: 1, own_machine_subscriptions: [ownMachine] });
    mine.mockResolvedValueOnce({ ok: true, value: before }); // the page's load
    sync.mockResolvedValue({ ok: true, value: before }); // not yet
    mine.mockResolvedValue({
      ok: true,
      value: subscribed(2, {
        own_machines: 1,
        own_machine_subscriptions: [ownMachine],
        static_addresses: 1,
        address_subscriptions: [addressSubscription({ address_id: null })],
      }),
    });
    await mountAt('/me/billing?checkout=success&session_id=cs_test_1');
    expect(statusText()).toBe('Confirming your payment with Stripe…');
    await vi.advanceTimersByTimeAsync(2_000);
    await flushPromises();
    expect(statusText()).toBe('Your static address is paid for: name it below.');
    expect(router.currentRoute.value.fullPath).toBe('/me/billing');
  });

  it('confirms another dedicated machine only once the account has one more', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout', 'Date'] });
    window.sessionStorage.setItem('opennova_checkout', JSON.stringify({ item: 'own_machine', own_machines_before: 1 }));
    const before = subscribed(3, { own_machines: 1, own_machine_subscriptions: [ownMachine] });
    mine.mockResolvedValueOnce({ ok: true, value: before }); // the page's load
    sync.mockResolvedValue({ ok: true, value: before }); // its first machine: not yet
    const newer = ownMachineSubscription({ stripe_subscription_id: 'sub_own_2' });
    mine.mockResolvedValue({ ok: true, value: subscribed(3, { own_machines: 2, own_machine_subscriptions: [newer, ownMachine] }) });
    await mountAt('/me/billing?checkout=success&session_id=cs_test_1');
    expect(statusText()).toBe('Confirming your payment with Stripe…');
    await vi.advanceTimersByTimeAsync(2_000);
    await flushPromises();
    expect(statusText()).toContain('Your dedicated machine is active.');
    expect(router.currentRoute.value.fullPath).toBe('/me/billing');
  });

  it("adds dedicated machines past the tier's servers: only the fleet caps them", async () => {
    // Tier 1 and a dedicated machine already: 2 servers, and another is on offer.
    mine.mockResolvedValue({ ok: true, value: subscribed(1, { max_servers: 2, own_machines: 1, own_machine_subscriptions: [ownMachine] }) });
    const page = await mountAt('/me/billing');
    const add = page.findAll('button').find((b) => b.text() === 'Add another')!;
    expect(add.attributes('disabled')).toBeUndefined();
    expect(page.findAll('button').some((b) => b.text() === 'Cancel in Stripe')).toBe(true);
  });

  it('warns while a payment is past due', async () => {
    mine.mockResolvedValue({
      ok: true,
      value: subscribed(1, { can_start: false, subscription: subscription({ tier: 1, status: 'past_due' }) }),
    });
    const page = await mountAt('/me/billing');
    expect(page.get('#billing-past-due').text()).toContain("new servers can't start");
    portal.mockResolvedValue({ ok: true, value: PORTAL_URL });
    await page.get('#billing-past-due button').trigger('click');
    await flushPromises();
    expect(portal).toHaveBeenCalledWith();
    expect(navigate).toHaveBeenCalledWith(PORTAL_URL);
  });

  it("warns while a dedicated machine's payment is past due, the tier's paid", async () => {
    mine.mockResolvedValue({
      ok: true,
      value: subscribed(2, {
        can_start: false,
        own_machines: 1,
        own_machine_subscriptions: [ownMachineSubscription({ status: 'past_due' })],
      }),
    });
    const page = await mountAt('/me/billing');
    expect(page.find('#billing-past-due').exists()).toBe(true);
    // No dedicated machine sells meanwhile.
    expect(page.findAll('button').find((b) => b.text() === 'Add another')!.attributes('disabled')).toBeDefined();
    expect(page.get('#billing-own-machine-block').text()).toBe('Fix the past-due payment first.');
  });

  it("says a canceled tier's end stops the servers on shared machines", async () => {
    mine.mockResolvedValue({ ok: true, value: subscribed(2, { subscription: subscription({ tier: 2, cancel_at_period_end: true }) }) });
    const page = await mountAt('/me/billing');
    expect(page.get('#billing-canceled').text()).toBe('Canceled: when it ends, your servers on shared machines stop.');
    expect(page.get('#billing-tier').text()).toContain('Ends');
  });

  it("says the account's grant keeps the servers of a tier that ends", async () => {
    mine.mockResolvedValue({
      ok: true,
      value: subscribed(2, {
        max_servers: 5,
        comp_servers: 3,
        granted: true,
        subscription: subscription({ tier: 2, cancel_at_period_end: true }),
      }),
    });
    const page = await mountAt('/me/billing');
    expect(page.get('#billing-canceled').text()).toBe('Canceled: when it ends, your grant keeps up to 3 servers running.');
  });

  it('opens the billing portal from Manage in Stripe, in the page header while the account has a Stripe customer', async () => {
    const none = await mountAt('/me/billing');
    expect(none.find('.page-header button').exists()).toBe(false);
    wrapper?.unmount();

    mine.mockResolvedValue({ ok: true, value: subscribed(2) });
    portal.mockResolvedValue({ ok: true, value: PORTAL_URL });
    const page = await mountAt('/me/billing');
    expect(page.findAll('.page-header button').map((b) => b.text())).toEqual(['Manage in Stripe']);
    await page.get('.page-header button').trigger('click');
    await flushPromises();
    expect(portal).toHaveBeenCalledWith();
    expect(navigate).toHaveBeenCalledWith(PORTAL_URL);
  });

  it('says no payment was taken after a canceled checkout, and drops the query', async () => {
    const page = await mountAt('/me/billing?checkout=cancel');
    expect(page.text()).toContain('No payment was taken.');
    expect(router.currentRoute.value.fullPath).toBe('/me/billing');
    expect(sync).not.toHaveBeenCalled();
    // Its hold on the fleet's room goes at once.
    expect(cancel).toHaveBeenCalledTimes(1);
  });

  it('confirms a checkout: syncs, polls until the tier is active, then drops the query', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout', 'Date'] });
    sync.mockResolvedValue({ ok: true, value: myBilling() });
    mine
      .mockResolvedValueOnce({ ok: true, value: myBilling() }) // the page's load
      .mockResolvedValueOnce({ ok: true, value: myBilling() }) // 2 s: not yet
      .mockResolvedValue({ ok: true, value: subscribed(2) }); // 3 s later: active
    const page = await mountAt('/me/billing?checkout=success&session_id=cs_test_1');
    expect(sync).toHaveBeenCalledTimes(1);
    expect(statusText()).toBe('Confirming your payment with Stripe…');
    expect(router.currentRoute.value.query.checkout).toBe('success');

    await vi.advanceTimersByTimeAsync(2_000);
    await flushPromises();
    expect(mine).toHaveBeenCalledTimes(2);
    expect(statusText()).toBe('Confirming your payment with Stripe…');

    await vi.advanceTimersByTimeAsync(3_000);
    await flushPromises();
    expect(mine).toHaveBeenCalledTimes(3);
    expect(statusText()).toContain('Your tier is active');
    expect(router.currentRoute.value.fullPath).toBe('/me/billing');
    expect(page.get('#billing-tier-2').attributes('aria-current')).toBe('true');
  });

  it('notes a confirmation still pending after a minute, then checks every 30 s', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout', 'Date'] });
    sync.mockResolvedValue({ ok: true, value: myBilling() });
    await mountAt('/me/billing?checkout=success&session_id=cs_test_1');
    await vi.advanceTimersByTimeAsync(60_000);
    await flushPromises();
    const reads = mine.mock.calls.length;
    // 2 s, 3 s, 4.5 s, then every 5 s up to the minute.
    expect(reads).toBeGreaterThanOrEqual(10);
    await vi.advanceTimersByTimeAsync(5_000);
    await flushPromises();
    expect(statusText()).toContain('Your payment went through');
    const pending = mine.mock.calls.length;
    await vi.advanceTimersByTimeAsync(29_000);
    expect(mine.mock.calls.length).toBe(pending);
    await vi.advanceTimersByTimeAsync(1_000);
    expect(mine.mock.calls.length).toBe(pending + 1);
    expect(router.currentRoute.value.query.checkout).toBe('success');
  });

  it('claims no payment when the sync finds no billing account', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout', 'Date'] });
    sync.mockResolvedValue({
      ok: false,
      error: { code: 'no_billing_account', status: 404, message: 'You have no billing account yet.' },
    });
    await mountAt('/me/billing?checkout=success&session_id=cs_test_1');
    await flushPromises();
    expect(router.currentRoute.value.fullPath).toBe('/me/billing');
    await vi.advanceTimersByTimeAsync(120_000);
    await flushPromises();
    expect(statusText()).not.toContain('Your payment went through');
    expect(statusText()).not.toContain('Confirming');
    expect(mine).toHaveBeenCalledTimes(1); // the page's load alone
  });

  it('holds the confirmation back while the page could not read the account', async () => {
    mine.mockResolvedValueOnce({ ok: false, error: { code: 'network', status: null, message: 'Could not reach the server.' } });
    const page = await mountAt('/me/billing?checkout=success&session_id=cs_test_1');
    expect(sync).not.toHaveBeenCalled();
    expect(page.text()).toContain('Could not reach the server.');
    expect(router.currentRoute.value.query.checkout).toBe('success');
    // Read again: the confirmation starts.
    sync.mockResolvedValue({ ok: true, value: subscribed(2) });
    await page.findAll('button').find((b) => b.text() === 'Try again')!.trigger('click');
    await flushPromises();
    expect(sync).toHaveBeenCalledTimes(1);
    expect(statusText()).toContain('Your tier is active');
  });

  it('says billing is not open with billing off', async () => {
    tiers.mockResolvedValue({ ok: true, value: tiersResponse({ enabled: false }) });
    mine.mockResolvedValue({
      ok: false,
      error: { code: 'billing_disabled', status: 503, message: 'Support tiers are not open on NovaWorld yet.' },
    });
    const page = await mountAt('/me/billing?checkout=success&session_id=cs_test_1');
    expect(page.get('.billing-closed').text()).toBe('Billing is not open yet.');
    expect(page.findAll('button').some((b) => b.text() === 'Try again')).toBe(false);
    expect(page.find('[role="alert"]').text()).toBe('');
    expect(sync).not.toHaveBeenCalled();
    expect(router.currentRoute.value.fullPath).toBe('/me/billing');
  });
});

describe('BillingPage static addresses', () => {
  const ownMachine = ownMachineSubscription();

  beforeEach(() => {
    vi.clearAllMocks();
    window.sessionStorage.clear();
    tiers.mockResolvedValue({ ok: true, value: tiersResponse({ static_address: { amount: 500, interval: 'month', available: true } }) });
    mine.mockResolvedValue({
      ok: true,
      value: subscribed(2, {
        own_machines: 2,
        own_machine_subscriptions: [ownMachineSubscription({ quantity: 2 })],
        static_addresses: 1,
        address_subscriptions: [addressSubscription({ stripe_subscription_id: 'sub_address_1', address_id: 1 })],
      }),
    });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    vi.useRealTimers();
  });

  function row(id: number) {
    return wrapper!.get(`#addresses tr#address-${id}`);
  }

  it('lists each address with where players join it, its server, state and subscription', async () => {
    addresses.mockResolvedValue({
      ok: true,
      value: addressesResponse([
        address(1, 'smoke', { server: { id: 31, game_name: 'Night Ops', state: 'running' } }),
        address(2, 'spare', { source: 'comp', stripe_subscription_id: null }),
        address(3, 'broken', { state: 'failed', public_ip: null, port: null, reason: 'the cloud has no address left' }),
      ]),
    });
    const page = await mountAt('/me/billing#addresses');
    expect(page.findAll('#addresses thead th').map((th) => th.text())).toEqual(['Name', 'Address', 'Server', 'State', 'Paid', 'Actions']);
    expect(row(1).text()).toContain('smoke.play.opennova.net');
    expect(row(1).text()).toContain('203.0.113.1:32768');
    expect(row(1).text()).toContain('Ready');
    expect(row(1).text()).toMatch(/Renews [A-Z][a-z]+ \d+, 2026/);
    expect(row(1).get('a').attributes('href')).toBe('/me/servers/31');
    // An admin's grant: no cancel.
    expect(row(2).text()).toContain('Granted');
    expect(row(2).findAll('button').some((b) => b.text().startsWith('Cancel'))).toBe(false);
    expect(row(1).findAll('button').some((b) => b.text().startsWith('Cancel'))).toBe(true);
    expect(row(3).text()).toContain('Could not be set up');
    expect(row(3).text()).toContain('the cloud has no address left');
    expect(row(3).text()).toContain('Not given yet');
    expect(page.find('#address-name').exists()).toBe(false);
  });

  it('copies the address players join', async () => {
    const writeText = vi.fn(() => Promise.resolve());
    Object.defineProperty(navigator, 'clipboard', { value: { writeText }, configurable: true });
    addresses.mockResolvedValue({ ok: true, value: addressesResponse([address(1, 'smoke')]) });
    const page = await mountAt('/me/billing');
    await row(1).findAll('button').find((b) => b.text().startsWith('Copy'))!.trigger('click');
    await flushPromises();
    expect(writeText).toHaveBeenCalledWith('203.0.113.1:32768');
    expect(page.get('[role="status"]').text()).toBe('Copied 203.0.113.1:32768.');
    Object.defineProperty(navigator, 'clipboard', { value: undefined, configurable: true });
  });

  it('names a bought address, the rule showing only as the field error', async () => {
    addresses.mockResolvedValue({ ok: true, value: addressesResponse([], { unnamed: 1 }) });
    const page = await mountAt('/me/billing');
    expect(page.get('label[for="address-name"]').text()).toBe('Name your new address');
    expect(page.get('#addresses').text()).toContain('.play.opennova.net');
    expect(page.find('#address-name-error').exists()).toBe(false);
    expect(page.get('#addresses').text()).not.toContain('lower-case');

    await page.get('#address-name').setValue('Night_Ops');
    await page.get('#addresses form').trigger('submit');
    await flushPromises();
    expect(name).not.toHaveBeenCalled();
    expect(page.get('#address-name').attributes('aria-invalid')).toBe('true');
    expect(page.get('#address-name').attributes('aria-describedby')).toBe('address-name-error');
    expect(page.get('#address-name-error').text()).toContain('lower-case letters');

    name.mockResolvedValueOnce({
      ok: false,
      error: { code: 'name_taken', status: 409, message: 'Another static address already has that name. Pick another.' },
    });
    await page.get('#address-name').setValue('smoke');
    await page.get('#addresses form').trigger('submit');
    await flushPromises();
    expect(name).toHaveBeenCalledWith('smoke');
    expect(page.get('#address-name-error').text()).toBe('Another static address already has that name. Pick another.');

    name.mockResolvedValueOnce({ ok: true, value: address(4, 'night-ops', { state: 'allocating', public_ip: null }) });
    addresses.mockResolvedValue({ ok: true, value: addressesResponse([address(4, 'night-ops', { state: 'allocating', public_ip: null })]) });
    await page.get('#address-name').setValue('night-ops');
    await page.get('#addresses form').trigger('submit');
    await flushPromises();
    expect(page.get('[role="status"]').text()).toBe('night-ops.play.opennova.net is being set up.');
    expect(row(4).text()).toContain('Being set up');
    expect(page.find('#address-name').exists()).toBe(false);
  });

  it("cancels a subscription's address through the portal's cancel flow", async () => {
    addresses.mockResolvedValue({ ok: true, value: addressesResponse([address(1, 'smoke')]) });
    portal.mockResolvedValue({ ok: true, value: PORTAL_URL });
    await mountAt('/me/billing');
    await row(1).findAll('button').find((b) => b.text().startsWith('Cancel'))!.trigger('click');
    await flushPromises();
    expect(portal).toHaveBeenCalledWith('sub_address_1');
    expect(navigate).toHaveBeenCalledWith(PORTAL_URL);
  });

  it('offers no cancel once the subscription is canceled, saying when it ends', async () => {
    mine.mockResolvedValue({
      ok: true,
      value: subscribed(2, {
        own_machines: 1,
        own_machine_subscriptions: [ownMachine],
        static_addresses: 1,
        address_subscriptions: [addressSubscription({ cancel_at_period_end: true })],
      }),
    });
    addresses.mockResolvedValue({ ok: true, value: addressesResponse([address(1, 'smoke')]) });
    await mountAt('/me/billing');
    expect(row(1).text()).toMatch(/Ends [A-Z][a-z]+ \d+, 2026/);
    expect(row(1).findAll('button').some((b) => b.text().startsWith('Cancel'))).toBe(false);
  });

  it('reads again while an address is being set up', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
    addresses.mockResolvedValueOnce({ ok: true, value: addressesResponse([address(1, 'smoke', { state: 'allocating', public_ip: null })]) });
    addresses.mockResolvedValue({ ok: true, value: addressesResponse([address(1, 'smoke')]) });
    await mountAt('/me/billing');
    expect(row(1).text()).toContain('Being set up');
    await vi.advanceTimersByTimeAsync(5_000);
    await flushPromises();
    expect(row(1).text()).toContain('Ready');
    // Nothing moves on its own now: no further read.
    const reads = addresses.mock.calls.length;
    await vi.advanceTimersByTimeAsync(60_000);
    expect(addresses.mock.calls.length).toBe(reads);
  });

  it('says when the account has none', async () => {
    addresses.mockResolvedValue({ ok: true, value: addressesResponse([]) });
    const page = await mountAt('/me/billing');
    expect(page.get('#addresses tbody').text()).toBe('No static addresses yet.');
  });

  it('shows a granted address even with nothing on sale', async () => {
    tiers.mockResolvedValue({ ok: true, value: tiersResponse({ on_sale: false }) });
    addresses.mockResolvedValue({ ok: true, value: addressesResponse([address(2, 'spare', { source: 'comp', stripe_subscription_id: null })]) });
    const page = await mountAt('/me/billing');
    expect(page.find('#addresses-add-block').exists()).toBe(false);
    expect(page.findAll('#addresses button').some((b) => b.text() === 'Add a static address')).toBe(false);
    expect(row(2).text()).toContain('spare.play.opennova.net');
  });
});
