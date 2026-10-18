// Tiers and account billing for the support tier tests, shaped as GET
// /api/billing/tiers and GET /api/me/billing answer them.

import type { MyBilling, TierSubscription, TiersResponse } from '../types/billing';

export function tiersResponse(overrides: Partial<TiersResponse> = {}): TiersResponse {
  return {
    enabled: true,
    currency: 'usd',
    tiers: [
      { tier: 1, max_servers: 1, amount: 500, interval: 'month' },
      { tier: 2, max_servers: 2, amount: 1000, interval: 'month' },
      { tier: 3, max_servers: 3, amount: 1500, interval: 'month' },
    ],
    own_machine: { amount: 4500, interval: 'month' },
    ...overrides,
  };
}

export function subscription(overrides: Partial<TierSubscription> = {}): TierSubscription {
  return {
    tier: 2,
    status: 'active',
    entitled: true,
    current_period_end: '2026-11-14T12:00:00Z',
    cancel_at_period_end: false,
    ...overrides,
  };
}

// An account with no tier, unless the overrides give it one.
export function myBilling(overrides: Partial<MyBilling> = {}): MyBilling {
  return {
    enabled: true,
    max_servers: 0,
    comp_servers: 0,
    comp_own_machines: 0,
    own_machines: 0,
    used: 0,
    stopping: 0,
    can_start: false,
    subscription: null,
    own_machine_subscription: null,
    can_manage: false,
    can_subscribe: true,
    ...overrides,
  };
}

// An account on `tier` (its servers' worth), active.
export function subscribed(tier: 1 | 2 | 3, overrides: Partial<MyBilling> = {}): MyBilling {
  return myBilling({
    max_servers: tier,
    can_start: true,
    subscription: subscription({ tier }),
    can_manage: true,
    can_subscribe: false,
    ...overrides,
  });
}
