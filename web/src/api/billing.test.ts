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
import { myBilling, subscribed, tiersResponse } from '../test/billingFixtures';
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

  it("reads the account's billing and refuses a malformed one", () => {
    expect(parseMyBilling(myBilling())).toEqual(myBilling());
    expect(parseMyBilling(subscribed(2))).toEqual(subscribed(2));
    expect(parseMyBilling({ ...myBilling(), used: -1 })).toBeNull();
    expect(parseMyBilling({ ...myBilling(), can_manage: 'yes' })).toBeNull();
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
  });

  it('maps the refusals to messages, never the server text', async () => {
    const cases: [number, string, string][] = [
      [409, 'already_subscribed', 'You already have a support tier'],
      [409, 'own_machine_limit', 'an own machine for every server'],
      [409, 'checkout_in_progress', 'already starting'],
      [503, 'billing_disabled', 'not open on NovaWorld yet'],
      [503, 'billing_misconfigured', 'no payment was taken'],
      [503, 'stripe_unavailable', 'Stripe could not take the request'],
      [403, 'not_eligible', 'cannot buy that right now'],
      [404, 'no_billing_account', 'subscribe to a support tier first'],
      [400, 'invalid_item', 'could not read the request'],
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
