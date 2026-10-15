import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createMemoryHistory, createRouter, type Router } from 'vue-router';
import SupportTierPage from './SupportTierPage.vue';
import { fetchMyBilling, fetchTiers, openPortal, startCheckout, syncMyBilling } from '../../api/billing';
import { myBilling, subscribed, subscription, tiersResponse } from '../../test/billingFixtures';
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
    const add = page.findAll('button').find((b) => b.text() === 'Add another own machine');
    expect(add).toBeDefined();
    await add!.trigger('click');
    await flushPromises();
    expect(checkout).toHaveBeenCalledWith('own_machine');
  });

  it('hides the add-on once every server has an own machine', async () => {
    mine.mockResolvedValue({ ok: true, value: subscribed(1, { own_machines: 1, own_machine_subscription: ownMachine }) });
    const page = await mountAt('/me/support');
    expect(page.findAll('button').some((b) => b.text().includes('own machine'))).toBe(false);
    expect(page.get('#support-own-machine-hint').text()).toContain('an own machine for every server your tier covers');
    expect(page.findAll('button').some((b) => b.text() === 'Change how many, or cancel')).toBe(true);
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
