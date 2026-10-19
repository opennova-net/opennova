import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createMemoryHistory, createRouter, type Router } from 'vue-router';
import SupportTierPage from './SupportTierPage.vue';
import { fetchMyBilling, fetchTiers, openPortal, startCheckout, syncMyBilling } from '../../api/billing';
import { addressSubscription, myBilling, subscribed, subscription, tiersResponse } from '../../test/billingFixtures';
import { navigateToStripe } from '../../utils/navigate';

vi.mock('../../api/billing', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../api/billing')>()),
  fetchTiers: vi.fn(),
  fetchMyBilling: vi.fn(),
  syncMyBilling: vi.fn(),
  startCheckout: vi.fn(),
  openPortal: vi.fn(),
}));
vi.mock('../../utils/navigate', () => ({ navigateToStripe: vi.fn(() => true) }));

const tiers = vi.mocked(fetchTiers);
const mine = vi.mocked(fetchMyBilling);
const sync = vi.mocked(syncMyBilling);
const checkout = vi.mocked(startCheckout);
const portal = vi.mocked(openPortal);
const navigate = vi.mocked(navigateToStripe);

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
  wrapper = mount(SupportTierPage, { global: { plugins: [router] } });
  await flushPromises();
  return wrapper;
}

function statusText(): string {
  return wrapper!.get('[role="status"]').text();
}

describe('SupportTierPage', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    window.sessionStorage.clear(); // the checkout item a test's checkout remembered
    tiers.mockResolvedValue({ ok: true, value: tiersResponse() });
    mine.mockResolvedValue({ ok: true, value: myBilling() });
  });

  afterEach(() => {
    wrapper?.unmount();
    wrapper = null;
    vi.useRealTimers();
  });

  it('subscribes through a Stripe Checkout', async () => {
    checkout.mockResolvedValue({ ok: true, value: CHECKOUT_URL });
    const page = await mountAt('/me/support');
    expect(page.findAll('article[aria-current]')).toHaveLength(0);
    await page.get('#support-tier-2 button').trigger('click');
    await flushPromises();
    expect(checkout).toHaveBeenCalledWith('tier2');
    expect(navigate).toHaveBeenCalledWith(CHECKOUT_URL);
  });

  it("marks the account's tier and sends every change to the billing portal", async () => {
    mine.mockResolvedValue({ ok: true, value: subscribed(2, { used: 2 }) });
    portal.mockResolvedValue({ ok: true, value: PORTAL_URL });
    const page = await mountAt('/me/support');
    expect(page.get('#support-tier-2').attributes('aria-current')).toBe('true');
    expect(page.get('#support-tier-1').attributes('aria-current')).toBeUndefined();
    expect(page.get('#support-current').element.closest('section')!.textContent).toContain('Tier 2');
    // Moving down from 2 live servers to Tier 1 stops the newest.
    expect(page.get('#support-tier-1').text()).toContain('Your newest server will be stopped when the change takes effect.');
    await page.get('#support-tier-3 button').trigger('click');
    await flushPromises();
    expect(checkout).not.toHaveBeenCalled();
    expect(portal).toHaveBeenCalledTimes(1);
    expect(navigate).toHaveBeenCalledWith(PORTAL_URL);
  });

  it('counts no stopping server in what a move down would stop', async () => {
    // Two live on Tier 2, one of them stopping already: Tier 1 keeps the other.
    mine.mockResolvedValue({ ok: true, value: subscribed(2, { used: 2, stopping: 1 }) });
    const page = await mountAt('/me/support');
    expect(page.get('#support-tier-1').text()).not.toContain('will be stopped');
  });

  it('opens the portal when the checkout finds a tier already live', async () => {
    checkout.mockResolvedValue({
      ok: false,
      error: { code: 'already_subscribed', status: 409, message: 'You already have a support tier.' },
    });
    portal.mockResolvedValue({ ok: true, value: PORTAL_URL });
    const page = await mountAt('/me/support');
    await page.get('#support-tier-1 button').trigger('click');
    await flushPromises();
    expect(portal).toHaveBeenCalledTimes(1);
    expect(navigate).toHaveBeenCalledWith(PORTAL_URL);
  });

  it('says what is wrong when Stripe\'s address looks wrong', async () => {
    checkout.mockResolvedValue({ ok: true, value: 'https://evil.example/' });
    navigate.mockReturnValueOnce(false);
    const page = await mountAt('/me/support');
    await page.get('#support-tier-1 button').trigger('click');
    await flushPromises();
    expect(page.get('[role="alert"]').text()).toContain('looked wrong');
  });

  const ownMachine = { quantity: 1, status: 'active', current_period_end: '2026-11-14T12:00:00Z', cancel_at_period_end: false };

  it('offers another own machine below the limit', async () => {
    mine.mockResolvedValue({ ok: true, value: subscribed(3, { own_machines: 1, own_machine_subscription: ownMachine }) });
    checkout.mockResolvedValue({ ok: true, value: CHECKOUT_URL });
    const page = await mountAt('/me/support');
    const section = page.get('#support-own-machine').element.closest('section')!;
    expect(section.textContent).toContain('You have 1 own machine (active).');
    expect(page.get('#support-own-machine-hint').text()).toContain('cancel one in the billing portal');
    const add = page.findAll('button').find((b) => b.text() === 'Add another own machine');
    expect(add).toBeDefined();
    await add!.trigger('click');
    await flushPromises();
    expect(checkout).toHaveBeenCalledWith('own_machine');
    // The count it had goes along, for the return to wait for one more.
    expect(JSON.parse(window.sessionStorage.getItem('opennova_checkout')!)).toEqual({
      item: 'own_machine',
      own_machines_before: 1,
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
    mine.mockResolvedValue({ ok: true, value: subscribed(3, { own_machines: 1, own_machine_subscription: ownMachine }) });
    const page = await mountAt('/me/support');
    // The account's own tier keeps its Manage while the fleet is full.
    expect(page.get('#support-tier-3 button').attributes('disabled')).toBeUndefined();
    expect(page.get('#support-tier-3').text()).not.toContain('Hosting is full');
    const add = page.findAll('button').find((b) => b.text() === 'Add another own machine')!;
    expect(add.attributes('disabled')).toBeDefined();
    expect(page.get('#support-own-machine-hint').text()).toBe('Hosting is full: new subscriptions are paused.');
    expect(page.findAll('button').some((b) => b.text() === 'Cancel one' && b.attributes('disabled') === undefined)).toBe(true);
  });

  it('reads the tiers again when a checkout finds the fleet sold out', async () => {
    checkout.mockResolvedValue({
      ok: false,
      error: { code: 'sold_out', status: 409, message: 'Hosting is full: new subscriptions are paused. Try again later; no payment was taken.' },
    });
    const page = await mountAt('/me/support');
    const offer = tiersResponse();
    tiers.mockResolvedValue({ ok: true, value: tiersResponse({ tiers: offer.tiers.map((t) => ({ ...t, available: false })) }) });
    await page.get('#support-tier-1 button').trigger('click');
    await flushPromises();
    expect(page.get('[role="alert"]').text()).toContain('no payment was taken');
    expect(tiers).toHaveBeenCalledTimes(2);
    expect(page.get('#support-tier-1').text()).toContain('Hosting is full: new subscriptions are paused.');
    expect(page.get('#support-tier-1 button').attributes('disabled')).toBeDefined();
    expect(navigate).not.toHaveBeenCalled();
  });

  const STATIC_ADDRESS = { amount: 500, interval: 'month', available: true };

  it('sells a static address for each own machine, and links to the addresses', async () => {
    tiers.mockResolvedValue({ ok: true, value: tiersResponse({ static_address: STATIC_ADDRESS }) });
    mine.mockResolvedValue({
      ok: true,
      value: subscribed(3, {
        own_machines: 2,
        own_machine_subscription: { ...ownMachine, quantity: 2 },
        static_addresses: 1,
        address_subscriptions: [addressSubscription()],
      }),
    });
    checkout.mockResolvedValue({ ok: true, value: CHECKOUT_URL });
    const page = await mountAt('/me/support');
    const section = page.get('#support-static-address').element.closest('section')!;
    expect(section.textContent).toContain('$5');
    expect(section.textContent).toContain('You have 1 static address.');
    expect(page.findAll('a').some((a) => a.attributes('href') === '/me/addresses')).toBe(true);
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
    // No own machine.
    mine.mockResolvedValue({ ok: true, value: subscribed(2) });
    let page = await mountAt('/me/support');
    const button = () => page.findAll('button').find((b) => b.text() === 'Add a static address')!;
    expect(button().attributes('disabled')).toBeDefined();
    expect(page.get('#support-static-address-hint').text()).toContain('Needs an own machine first');
    page.unmount();

    // One own machine, already with its address.
    mine.mockResolvedValue({
      ok: true,
      value: subscribed(2, { own_machines: 1, own_machine_subscription: ownMachine, static_addresses: 1, address_subscriptions: [addressSubscription()] }),
    });
    page = await mountAt('/me/support');
    expect(button().attributes('disabled')).toBeDefined();
    expect(page.get('#support-static-address-hint').text()).toContain('a static address for every own machine');
    page.unmount();

    // Room for one, and the fleet sold out of addresses.
    tiers.mockResolvedValue({ ok: true, value: tiersResponse({ static_address: { ...STATIC_ADDRESS, available: false } }) });
    mine.mockResolvedValue({ ok: true, value: subscribed(2, { own_machines: 1, own_machine_subscription: ownMachine }) });
    page = await mountAt('/me/support');
    expect(button().attributes('disabled')).toBeDefined();
    expect(page.get('#support-static-address-hint').text()).toBe('Hosting is full: new subscriptions are paused.');
  });

  it('says what an admin granted and offers no tier or add-on beside it', async () => {
    tiers.mockResolvedValue({ ok: true, value: tiersResponse({ static_address: STATIC_ADDRESS }) });
    mine.mockResolvedValue({
      ok: true,
      value: myBilling({
        max_servers: 11,
        comp_servers: 11,
        comp_own_machines: 2,
        comp_addresses: 1,
        granted: true,
        own_machines: 2,
        can_start: true,
        can_subscribe: false,
      }),
    });
    const page = await mountAt('/me/support');
    expect(page.get('#support-granted').text()).toBe(
      'An admin granted your account 11 hosted servers, 2 own machines for your servers and 1 static address, free of charge. ' +
        'With an admin grant there is no support tier or add-on to buy.',
    );
    for (const id of ['#support-tiers', '#support-own-machine', '#support-static-address']) expect(page.find(id).exists()).toBe(false);
    expect(page.findAll('button').some((b) => /subscribe|own machine|static address/i.test(b.text()))).toBe(false);
  });

  it('shows no static address card where none is sold', async () => {
    const page = await mountAt('/me/support');
    expect(page.find('#support-static-address').exists()).toBe(false);
  });

  it('confirms a static address once one more of them is active', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout', 'Date'] });
    tiers.mockResolvedValue({ ok: true, value: tiersResponse({ static_address: STATIC_ADDRESS }) });
    window.sessionStorage.setItem('opennova_checkout', JSON.stringify({ item: 'static_address', addresses_before: 0 }));
    const before = subscribed(2, { own_machines: 1, own_machine_subscription: ownMachine });
    mine.mockResolvedValueOnce({ ok: true, value: before }); // the page's load
    sync.mockResolvedValue({ ok: true, value: before }); // not yet
    mine.mockResolvedValue({
      ok: true,
      value: subscribed(2, {
        own_machines: 1,
        own_machine_subscription: ownMachine,
        static_addresses: 1,
        address_subscriptions: [addressSubscription({ address_id: null })],
      }),
    });
    await mountAt('/me/support?checkout=success&session_id=cs_test_1');
    expect(statusText()).toBe('Confirming your payment with Stripe…');
    await vi.advanceTimersByTimeAsync(2_000);
    await flushPromises();
    expect(statusText()).toBe('Your static address is paid for. Give it a name on your Static addresses page.');
    expect(router.currentRoute.value.fullPath).toBe('/me/support');
  });

  it('confirms another own machine only once the account has one more', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout', 'Date'] });
    window.sessionStorage.setItem('opennova_checkout', JSON.stringify({ item: 'own_machine', own_machines_before: 1 }));
    const before = subscribed(3, { own_machines: 1, own_machine_subscription: ownMachine });
    mine.mockResolvedValueOnce({ ok: true, value: before }); // the page's load
    sync.mockResolvedValue({ ok: true, value: before }); // its first machine: not yet
    mine.mockResolvedValue({ ok: true, value: subscribed(3, { own_machines: 2, own_machine_subscription: ownMachine }) });
    await mountAt('/me/support?checkout=success&session_id=cs_test_1');
    expect(statusText()).toBe('Confirming your payment with Stripe…');
    await vi.advanceTimersByTimeAsync(2_000);
    await flushPromises();
    expect(statusText()).toContain('Your own machine is active.');
    expect(router.currentRoute.value.fullPath).toBe('/me/support');
  });

  it('hides the add-on once every server has an own machine', async () => {
    mine.mockResolvedValue({ ok: true, value: subscribed(1, { own_machines: 1, own_machine_subscription: ownMachine }) });
    const page = await mountAt('/me/support');
    expect(page.findAll('button').some((b) => b.text().includes('own machine'))).toBe(false);
    expect(page.get('#support-own-machine-hint').text()).toContain('an own machine for every server your tier covers');
    expect(page.findAll('button').some((b) => b.text() === 'Cancel one')).toBe(true);
  });

  it('warns while a payment is past due', async () => {
    mine.mockResolvedValue({
      ok: true,
      value: subscribed(1, { can_start: false, subscription: subscription({ tier: 1, status: 'past_due' }) }),
    });
    const page = await mountAt('/me/support');
    expect(page.get('#support-past-due').element.parentElement!.textContent).toContain(
      "Your servers keep running while Stripe retries the card; new servers can't start.",
    );
  });

  it('says no payment was taken after a canceled checkout, and drops the query', async () => {
    const page = await mountAt('/me/support?checkout=cancel');
    expect(page.text()).toContain('No payment was taken.');
    expect(router.currentRoute.value.fullPath).toBe('/me/support');
    expect(sync).not.toHaveBeenCalled();
  });

  it('confirms a checkout: syncs, polls until the tier is active, then drops the query', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout', 'Date'] });
    sync.mockResolvedValue({ ok: true, value: myBilling() });
    mine
      .mockResolvedValueOnce({ ok: true, value: myBilling() }) // the page's load
      .mockResolvedValueOnce({ ok: true, value: myBilling() }) // 2 s: not yet
      .mockResolvedValue({ ok: true, value: subscribed(2) }); // 3 s later: active
    const page = await mountAt('/me/support?checkout=success&session_id=cs_test_1');
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
    expect(statusText()).toContain('Your support tier is active.');
    expect(router.currentRoute.value.fullPath).toBe('/me/support');
    expect(page.get('#support-tier-2').attributes('aria-current')).toBe('true');
  });

  it('notes a confirmation still pending after a minute, then checks every 30 s', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout', 'Date'] });
    sync.mockResolvedValue({ ok: true, value: myBilling() });
    await mountAt('/me/support?checkout=success&session_id=cs_test_1');
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
    await mountAt('/me/support?checkout=success&session_id=cs_test_1');
    await flushPromises();
    expect(router.currentRoute.value.fullPath).toBe('/me/support');
    await vi.advanceTimersByTimeAsync(120_000);
    await flushPromises();
    expect(statusText()).not.toContain('Your payment went through');
    expect(statusText()).not.toContain('Confirming');
    expect(mine).toHaveBeenCalledTimes(1); // the page's load alone
  });

  it('holds the confirmation back while the page could not read the account', async () => {
    mine.mockResolvedValueOnce({ ok: false, error: { code: 'network', status: null, message: 'Could not reach the server.' } });
    const page = await mountAt('/me/support?checkout=success&session_id=cs_test_1');
    expect(sync).not.toHaveBeenCalled();
    expect(page.text()).toContain('Could not reach the server.');
    expect(router.currentRoute.value.query.checkout).toBe('success');
    // Read again: the confirmation starts.
    sync.mockResolvedValue({ ok: true, value: subscribed(2) });
    await page.findAll('button').find((b) => b.text() === 'Try again')!.trigger('click');
    await flushPromises();
    expect(sync).toHaveBeenCalledTimes(1);
    expect(statusText()).toContain('Your support tier is active.');
  });

  it('says support tiers are not open with billing off', async () => {
    tiers.mockResolvedValue({ ok: true, value: tiersResponse({ enabled: false }) });
    mine.mockResolvedValue({
      ok: false,
      error: { code: 'billing_disabled', status: 503, message: 'Support tiers are not open on NovaWorld yet.' },
    });
    const page = await mountAt('/me/support?checkout=success&session_id=cs_test_1');
    expect(page.text()).toContain('Support tiers are not open on NovaWorld yet.');
    expect(page.findAll('button').some((b) => b.text() === 'Try again')).toBe(false);
    expect(page.find('[role="alert"]').text()).toBe('');
    expect(sync).not.toHaveBeenCalled();
    expect(router.currentRoute.value.fullPath).toBe('/me/support');
  });
});
