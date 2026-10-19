import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createMemoryHistory, createRouter, type Router } from 'vue-router';
import SupportTierPage from './SupportTierPage.vue';
import { fetchMyBilling, fetchTiers, openPortal, startCheckout, syncMyBilling } from '../../api/billing';
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
    mine.mockResolvedValue({ ok: true, value: subscribed(2, { used: 2, shared_used: 2 }) });
    portal.mockResolvedValue({ ok: true, value: PORTAL_URL });
    const page = await mountAt('/me/support');
    expect(page.get('#support-tier-2').attributes('aria-current')).toBe('true');
    expect(page.get('#support-tier-1').attributes('aria-current')).toBeUndefined();
    const facts = page.findAll('#support-current ~ dl > *').map((f) => f.text());
    expect(facts[1]).toBe('Tier 2, up to 2 hosted servers at once on shared machines');
    expect(facts.slice(-2)).toEqual(['Live on shared machines', '2 of 2']);
    // Moving down from 2 live servers to Tier 1 stops the newest.
    expect(page.get('#support-tier-1').text()).toContain(
      'Your newest server on a shared machine will be stopped when the change takes effect.',
    );
    await page.get('#support-tier-3 button').trigger('click');
    await flushPromises();
    expect(checkout).not.toHaveBeenCalled();
    expect(portal).toHaveBeenCalledTimes(1);
    expect(navigate).toHaveBeenCalledWith(PORTAL_URL);
  });

  it('counts no stopping server in what a move down would stop', async () => {
    // Two live on Tier 2, one of them stopping already: Tier 1 keeps the other.
    mine.mockResolvedValue({ ok: true, value: subscribed(2, { used: 2, stopping: 1, shared_used: 2, shared_stopping: 1 }) });
    const page = await mountAt('/me/support');
    expect(page.get('#support-tier-1').text()).not.toContain('will be stopped');
  });

  it('counts no server on a dedicated machine in what a move down would stop', async () => {
    // Tier 2 with one server on a shared machine and one on a dedicated machine.
    mine.mockResolvedValue({
      ok: true,
      value: subscribed(2, { max_servers: 3, own_machines: 1, used: 2, shared_used: 1, own_machine_subscriptions: [ownMachineSubscription()] }),
    });
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

  const ownMachine = ownMachineSubscription();

  it('sells a dedicated machine with no support tier', async () => {
    checkout.mockResolvedValue({ ok: true, value: CHECKOUT_URL });
    const page = await mountAt('/me/support');
    const section = page.get('#support-own-machine').element.closest('section')!;
    expect(page.get('#support-own-machine').text()).toBe('Dedicated machines');
    expect(section.textContent).toContain(
      'One more hosted server, on a machine of its own. No support tier needed; add as many as you like, each its own monthly subscription.',
    );
    expect(section.textContent).toContain('$45');
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
    const page = await mountAt('/me/support');
    const section = page.get('#support-own-machine').element.closest('section')!;
    expect(section.textContent).toContain('You have 2 dedicated machines.');
    const rows = page.findAll('#support-own-machine-subscriptions li').map((li) => li.text().replace(/\s+/g, ' '));
    expect(rows).toHaveLength(2);
    expect(rows[0]).toMatch(/^1 dedicated machine: Active, ends .*2026$/);
    expect(rows[1]).toMatch(/^1 dedicated machine: Active, renews .*2026$/);
    expect(page.get('#support-own-machine-hint').text()).toContain('Cancel one in the billing portal');
    expect(page.findAll('button').some((b) => b.text() === 'Cancel one in the billing portal')).toBe(true);
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
    const page = await mountAt('/me/support');
    // The account's own tier keeps its Manage while the fleet is full.
    expect(page.get('#support-tier-3 button').attributes('disabled')).toBeUndefined();
    expect(page.get('#support-tier-3').text()).not.toContain('Hosting is full');
    const add = page.findAll('button').find((b) => b.text() === 'Add another')!;
    expect(add.attributes('disabled')).toBeDefined();
    expect(page.get('#support-own-machine-hint').text()).toBe('Hosting is full: new subscriptions are paused.');
    expect(
      page.findAll('button').some((b) => b.text() === 'Cancel one in the billing portal' && b.attributes('disabled') === undefined),
    ).toBe(true);
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

  it('sells a static address for each dedicated machine, and links to the addresses', async () => {
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
    // No dedicated machine.
    mine.mockResolvedValue({ ok: true, value: subscribed(2) });
    let page = await mountAt('/me/support');
    const button = () => page.findAll('button').find((b) => b.text() === 'Add a static address')!;
    expect(button().attributes('disabled')).toBeDefined();
    expect(page.get('#support-static-address-hint').text()).toContain('Needs a dedicated machine first');
    page.unmount();

    // One dedicated machine, already with its address.
    mine.mockResolvedValue({
      ok: true,
      value: subscribed(2, { own_machines: 1, own_machine_subscriptions: [ownMachine], static_addresses: 1, address_subscriptions: [addressSubscription()] }),
    });
    page = await mountAt('/me/support');
    expect(button().attributes('disabled')).toBeDefined();
    expect(page.get('#support-static-address-hint').text()).toContain('a static address for every dedicated machine');
    page.unmount();

    // Room for one, and the fleet sold out of addresses.
    tiers.mockResolvedValue({ ok: true, value: tiersResponse({ static_address: { ...STATIC_ADDRESS, available: false } }) });
    mine.mockResolvedValue({ ok: true, value: subscribed(2, { own_machines: 1, own_machine_subscriptions: [ownMachine] }) });
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
        shared_servers: 9,
        comp_servers: 9,
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
      'An admin granted your account 9 hosted servers on shared machines, 2 dedicated machines (one more server each) ' +
        'and 1 static address, free of charge. With an admin grant there is no support tier, dedicated machine or ' +
        'static address to buy.',
    );
    for (const id of ['#support-tiers', '#support-own-machine', '#support-static-address']) expect(page.find(id).exists()).toBe(false);
    expect(page.findAll('button').some((b) => /subscribe|dedicated machine|static address/i.test(b.text()))).toBe(false);
  });

  it('shows no static address card where none is sold', async () => {
    const page = await mountAt('/me/support');
    expect(page.find('#support-static-address').exists()).toBe(false);
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
    await mountAt('/me/support?checkout=success&session_id=cs_test_1');
    expect(statusText()).toBe('Confirming your payment with Stripe…');
    await vi.advanceTimersByTimeAsync(2_000);
    await flushPromises();
    expect(statusText()).toBe('Your static address is paid for. Give it a name on your Static addresses page.');
    expect(router.currentRoute.value.fullPath).toBe('/me/support');
  });

  it('confirms another dedicated machine only once the account has one more', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout', 'Date'] });
    window.sessionStorage.setItem('opennova_checkout', JSON.stringify({ item: 'own_machine', own_machines_before: 1 }));
    const before = subscribed(3, { own_machines: 1, own_machine_subscriptions: [ownMachine] });
    mine.mockResolvedValueOnce({ ok: true, value: before }); // the page's load
    sync.mockResolvedValue({ ok: true, value: before }); // its first machine: not yet
    const newer = ownMachineSubscription({ stripe_subscription_id: 'sub_own_2' });
    mine.mockResolvedValue({ ok: true, value: subscribed(3, { own_machines: 2, own_machine_subscriptions: [newer, ownMachine] }) });
    await mountAt('/me/support?checkout=success&session_id=cs_test_1');
    expect(statusText()).toBe('Confirming your payment with Stripe…');
    await vi.advanceTimersByTimeAsync(2_000);
    await flushPromises();
    expect(statusText()).toContain('Your dedicated machine is active.');
    expect(router.currentRoute.value.fullPath).toBe('/me/support');
  });

  it("adds dedicated machines past the tier's servers: only the fleet caps them", async () => {
    // Tier 1 and a dedicated machine already: 2 servers, and another is on offer.
    mine.mockResolvedValue({ ok: true, value: subscribed(1, { max_servers: 2, own_machines: 1, own_machine_subscriptions: [ownMachine] }) });
    const page = await mountAt('/me/support');
    const add = page.findAll('button').find((b) => b.text() === 'Add another')!;
    expect(add.attributes('disabled')).toBeUndefined();
    expect(page.findAll('button').some((b) => b.text() === 'Cancel one in the billing portal')).toBe(true);
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

  it("warns while an add-on's payment is past due, the tier's paid", async () => {
    mine.mockResolvedValue({
      ok: true,
      value: subscribed(2, {
        can_start: false,
        own_machines: 1,
        own_machine_subscriptions: [ownMachineSubscription({ status: 'past_due' })],
      }),
    });
    const page = await mountAt('/me/support');
    expect(page.get('#support-past-due').element.parentElement!.textContent).toContain(
      'Update your payment method to keep your subscriptions.',
    );
    // No dedicated machine sells meanwhile.
    expect(page.findAll('button').find((b) => b.text() === 'Add another')!.attributes('disabled')).toBeDefined();
    expect(page.get('#support-own-machine-hint').text()).toBe('Fix the payment that is past due first, then add one.');
  });

  it("says a tier's end stops the servers on shared machines and leaves the dedicated machines", async () => {
    mine.mockResolvedValue({
      ok: true,
      value: subscribed(2, { max_servers: 3, own_machines: 1, own_machine_subscriptions: [ownMachine] }),
    });
    let page = await mountAt('/me/support');
    expect(page.get('#support-cancel-hint').text()).toBe(
      "Change or cancel in Stripe's billing portal. A canceled tier lasts until the paid period ends, then your hosted " +
        'servers on shared machines stop. Your dedicated machines keep running: they need no tier.',
    );
    expect(page.get('#support-own-machine').element.closest('section')!.textContent).not.toContain('end with');
    page.unmount();

    // With no dedicated machine there is nothing more to say.
    mine.mockResolvedValue({ ok: true, value: subscribed(2) });
    page = await mountAt('/me/support');
    expect(page.get('#support-cancel-hint').text()).toBe(
      "Change or cancel in Stripe's billing portal. A canceled tier lasts until the paid period ends, then your hosted " +
        'servers on shared machines stop.',
    );
  });

  it("says an admin's grant keeps the servers of a tier that ends", async () => {
    // Granted after it subscribed: the tier box stays, canceled, with no add-on.
    mine.mockResolvedValue({
      ok: true,
      value: subscribed(2, {
        max_servers: 3,
        comp_servers: 3,
        granted: true,
        subscription: subscription({ tier: 2, cancel_at_period_end: true }),
      }),
    });
    const page = await mountAt('/me/support');
    const canceled = page.get('#support-canceled').text();
    expect(canceled).toBe(
      'Your tier is canceled: it lasts until the end of the period you paid for, then your admin grant keeps up to 3 ' +
        'hosted servers running on shared machines. You can renew it in the billing portal before then.',
    );
    expect(canceled).not.toContain('servers stop');
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
