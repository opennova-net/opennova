import { AxiosError, AxiosHeaders, type AxiosResponse } from 'axios';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { apiClient } from './client';
import {
  fetchMyBilling,
  fetchTiers,
  openPortal,
  parseMyBilling,
  parseTiers,
  startCheckout,
  syncMyBilling,
} from './billing';
import { addressSubscription, myBilling, ownMachineSubscription, subscribed, tiersResponse } from '../test/billingFixtures';
import { isStripeUrl, navigateToStripe } from '../utils/navigate';

vi.mock('./client', () => ({
  apiClient: { get: vi.fn(), post: vi.fn() },
  adminClient: { get: vi.fn(), post: vi.fn() },
}));

const get = vi.mocked(apiClient.get);
const post = vi.mocked(apiClient.post);

function reply(data: unknown, status = 200) {
  return { status, data } as AxiosResponse;
}

function refusal(status: number, data: unknown, headers: Record<string, string> = {}): AxiosError {
  const config = { headers: new AxiosHeaders() };
  return new AxiosError('refused', 'ERR_BAD_REQUEST', config, null, {
    status,
    statusText: '',
    data,
    headers,
    config,
  } as AxiosResponse);
}

describe('parsing', () => {
  it('reads the tiers in order and refuses a malformed one', () => {
    const body = tiersResponse();
    expect(parseTiers({ ...body, tiers: [body.tiers[2], body.tiers[0], body.tiers[1]] })?.tiers.map((t) => t.tier)).toEqual([
      1, 2, 3,
    ]);
    expect(parseTiers({ ...body, tiers: [{ ...body.tiers[0], amount: '5' }] })).toBeNull();
    expect(parseTiers({ ...body, tiers: [{ ...body.tiers[0], tier: 4 }] })).toBeNull();
    expect(parseTiers({ ...body, tiers: [body.tiers[0], body.tiers[0]] })).toBeNull();
    // A price Stripe has not answered for is null, and still a tier.
    expect(parseTiers({ ...body, tiers: [{ ...body.tiers[0], amount: null }] })?.tiers[0].amount).toBeNull();
    expect(parseTiers({ ...body, own_machine: null })?.own_machine).toBeNull();
    expect(parseTiers({ ...body, own_machine: { amount: 'x' } })).toBeNull();
  });

  it("reads each item's availability and the static address offer", () => {
    const body = tiersResponse();
    const full = parseTiers({
      ...body,
      tiers: [{ ...body.tiers[0], available: false }, body.tiers[1], body.tiers[2]],
      own_machine: { amount: 4500, interval: 'month', available: false },
      static_address: { amount: 500, interval: 'month', available: true },
    });
    expect(full?.tiers.map((t) => t.available)).toEqual([false, true, true]);
    expect(full?.own_machine?.available).toBe(false);
    expect(full?.static_address).toEqual({ amount: 500, interval: 'month', available: true });
    // Not sold here: null, or left out.
    expect(parseTiers({ ...body, static_address: null })?.static_address).toBeNull();
    const { static_address: _drop, ...none } = body;
    expect(parseTiers(none)?.static_address).toBeNull();
    // An item that does not say whether it is available is refused.
    const { available: _a, ...unsaid } = body.tiers[0];
    expect(parseTiers({ ...body, tiers: [unsaid] })).toBeNull();
    expect(parseTiers({ ...body, static_address: { amount: 500, interval: 'month' } })).toBeNull();
  });

  it("reads the account's static address subscriptions", () => {
    const value = myBilling({
      static_addresses: 1,
      address_subscriptions: [addressSubscription(), addressSubscription({ stripe_subscription_id: 'sub_2', address_id: null })],
    });
    expect(parseMyBilling(value)).toEqual(value);
    expect(parseMyBilling({ ...value, address_subscriptions: [{ ...addressSubscription(), address_id: 'one' }] })).toBeNull();
    const { static_addresses: _s, ...uncounted } = value;
    expect(parseMyBilling(uncounted)).toBeNull();
  });

  it("reads the account's dedicated machine subscriptions and its shared machines' counts", () => {
    // Tier 2 and two dedicated machines, no tier needed for them: 4 servers.
    const value = subscribed(2, {
      max_servers: 4,
      own_machines: 2,
      used: 3,
      shared_used: 2,
      shared_stopping: 1,
      stopping: 1,
      own_machine_subscriptions: [
        ownMachineSubscription({ stripe_subscription_id: 'sub_own_2', cancel_at_period_end: true }),
        ownMachineSubscription(),
      ],
    });
    expect(parseMyBilling(value)).toEqual(value);
    const { own_machine_subscriptions: _list, ...unlisted } = value;
    expect(parseMyBilling(unlisted)).toBeNull();
    const { stripe_subscription_id: _id, ...unnamed } = ownMachineSubscription();
    expect(parseMyBilling({ ...value, own_machine_subscriptions: [unnamed] })).toBeNull();
    expect(parseMyBilling({ ...value, own_machine_subscriptions: [ownMachineSubscription({ quantity: -1 })] })).toBeNull();
    const { shared_servers: _shared, ...uncounted } = value;
    expect(parseMyBilling(uncounted)).toBeNull();
    expect(parseMyBilling({ ...value, shared_stopping: 'one' })).toBeNull();
  });

  it("reads the account's billing and refuses a malformed one", () => {
    expect(parseMyBilling(myBilling())).toEqual(myBilling());
    expect(parseMyBilling(subscribed(2))).toEqual(subscribed(2));
    expect(parseMyBilling({ ...myBilling(), used: -1 })).toBeNull();
    expect(parseMyBilling({ ...myBilling(), can_manage: 'yes' })).toBeNull();
    expect(parseMyBilling({ ...myBilling(), granted: 'yes' })).toBeNull();
    expect(parseMyBilling({ ...myBilling(), comp_addresses: -1 })).toBeNull();
    const bad = subscribed(2);
    expect(parseMyBilling({ ...bad, subscription: { ...bad.subscription, current_period_end: 'soon' } })).toBeNull();
    expect(parseMyBilling({ ...bad, subscription: { ...bad.subscription, tier: 5 } })).toBeNull();
  });
});

describe('the routes', () => {
  beforeEach(() => vi.clearAllMocks());

  it('reads the tiers and the account', async () => {
    get.mockResolvedValueOnce(reply(tiersResponse()));
    expect((await fetchTiers()).ok).toBe(true);
    expect(get).toHaveBeenLastCalledWith('/billing/tiers', expect.anything());
    get.mockResolvedValueOnce(reply(subscribed(1)));
    const mine = await fetchMyBilling();
    expect(mine.ok && mine.value.subscription?.tier).toBe(1);
    expect(get).toHaveBeenLastCalledWith('/me/billing', expect.anything());
    post.mockResolvedValueOnce(reply(subscribed(1)));
    expect((await syncMyBilling()).ok).toBe(true);
    expect(post).toHaveBeenLastCalledWith('/me/billing/sync');
  });

  it("starts a checkout and opens the portal, answering Stripe's URL", async () => {
    post.mockResolvedValueOnce(reply({ url: 'https://checkout.stripe.com/c/pay/cs_test_1' }));
    expect(await startCheckout('tier2')).toEqual({ ok: true, value: 'https://checkout.stripe.com/c/pay/cs_test_1' });
    expect(post).toHaveBeenLastCalledWith('/billing/checkout', { item: 'tier2' });
    post.mockResolvedValueOnce(reply({ url: 'https://billing.stripe.com/p/session/test_1' }));
    expect((await openPortal()).ok).toBe(true);
    expect(post).toHaveBeenLastCalledWith('/billing/portal');
    post.mockResolvedValueOnce(reply({}));
    expect((await openPortal()).ok).toBe(false);
    // A static address's cancel flow names its subscription.
    post.mockResolvedValueOnce(reply({ url: 'https://billing.stripe.com/p/session/test_2' }));
    expect((await openPortal('sub_address_1')).ok).toBe(true);
    expect(post).toHaveBeenLastCalledWith('/billing/portal', { cancel_subscription: 'sub_address_1' });
  });

  it('reads a retired own_machine_limit as unknown: a dedicated machine has no cap but the fleet', async () => {
    post.mockRejectedValueOnce(refusal(409, { error: 'own_machine_limit' }));
    const result = await startCheckout('own_machine');
    expect(!result.ok && result.error.code).toBe('unknown');
  });

  it('maps the refusals to messages, never the server text', async () => {
    const cases: [number, string, string][] = [
      [409, 'already_subscribed', 'You already have a support tier'],
      [409, 'checkout_in_progress', 'already starting'],
      [503, 'billing_disabled', 'not open on NovaWorld yet'],
      [503, 'billing_misconfigured', 'no payment was taken'],
      [503, 'stripe_unavailable', 'Stripe could not take the request'],
      [403, 'not_eligible', 'an account with an admin grant buys no tier, dedicated machine or static address'],
      [404, 'no_billing_account', 'subscribe to a support tier or a dedicated machine first'],
      [400, 'invalid_item', 'could not read the request'],
      [409, 'sold_out', 'Hosting is full: new subscriptions are paused. Try again later; no payment was taken.'],
      [409, 'address_limit', 'a static address for every dedicated machine'],
      [403, 'payment_past_due', 'Fix it in the billing portal first'],
      [404, 'not_found', 'no longer open'],
    ];
    for (const [status, code, text] of cases) {
      post.mockRejectedValueOnce(refusal(status, { error: code, message: 'raw server text' }));
      const result = await startCheckout('tier1');
      expect(!result.ok && result.error.code).toBe(code);
      expect(!result.ok && result.error.message).toContain(text);
      expect(!result.ok && result.error.message).not.toContain('raw server text');
    }
    post.mockRejectedValueOnce(refusal(429, {}, { 'retry-after': '120' }));
    const limited = await openPortal();
    expect(!limited.ok && limited.error).toMatchObject({ code: 'rate_limited', retryAfterSeconds: 120 });
    expect(!limited.ok && limited.error.message).toContain('2 minutes');
  });
});

describe('navigateToStripe', () => {
  it("follows Stripe's Checkout and billing portal pages only", () => {
    const assign = vi.fn();
    expect(navigateToStripe('https://checkout.stripe.com/c/pay/cs_test_1#frag', assign)).toBe(true);
    expect(navigateToStripe('https://billing.stripe.com/p/session/test_1', assign)).toBe(true);
    expect(assign).toHaveBeenCalledTimes(2);
    assign.mockClear();
    for (const url of [
      'https://evil.example/c/pay',
      'http://checkout.stripe.com/c/pay',
      'https://checkout.stripe.com.evil.example/',
      'https://evil.example/?https://checkout.stripe.com',
      'https://user:pass@checkout.stripe.com/',
      'https://checkout.stripe.com:8443/',
      'https://dashboard.stripe.com/',
      'javascript:alert(1)',
      '/me/support',
      '',
    ]) {
      expect(isStripeUrl(url)).toBe(false);
      expect(navigateToStripe(url, assign)).toBe(false);
    }
    expect(assign).not.toHaveBeenCalled();
  });
});
