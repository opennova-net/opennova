// Tiers and account billing for the support tier tests, shaped as GET
// /api/billing/tiers and GET /api/me/billing answer them.

import type {
  AddressSubscription,
  MyBilling,
  OwnMachineSubscription,
  TierSubscription,
  TiersResponse,
} from '../types/billing';

export function tiersResponse(overrides: Partial<TiersResponse> = {}): TiersResponse {
  return {
    enabled: true,
    currency: 'usd',
    tiers: [
      { tier: 1, max_servers: 1, amount: 500, interval: 'month', available: true },
      { tier: 2, max_servers: 2, amount: 1000, interval: 'month', available: true },
      { tier: 3, max_servers: 3, amount: 1500, interval: 'month', available: true },
    ],
    own_machine: { amount: 4500, interval: 'month', available: true },
    static_address: null,
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

// A dedicated machine subscription of one machine, active.
export function ownMachineSubscription(overrides: Partial<OwnMachineSubscription> = {}): OwnMachineSubscription {
  return {
    stripe_subscription_id: 'sub_own_1',
    quantity: 1,
    status: 'active',
    current_period_end: '2026-11-14T12:00:00Z',
    cancel_at_period_end: false,
    ...overrides,
  };
}

// A static address's own subscription, named (its address 1) unless the
// overrides say otherwise.
export function addressSubscription(overrides: Partial<AddressSubscription> = {}): AddressSubscription {
  return {
    stripe_subscription_id: 'sub_address_1',
    status: 'active',
    current_period_end: '2026-11-14T12:00:00Z',
    cancel_at_period_end: false,
    address_id: 1,
    ...overrides,
  };
}

// An account with no tier, unless the overrides give it one.
export function myBilling(overrides: Partial<MyBilling> = {}): MyBilling {
  return {
    enabled: true,
    max_servers: 0,
    shared_servers: 0,
    comp_servers: 0,
    comp_own_machines: 0,
    comp_addresses: 0,
    granted: false,
    own_machines: 0,
    used: 0,
    stopping: 0,
    shared_used: 0,
    shared_stopping: 0,
    can_start: false,
    subscription: null,
    own_machine_subscriptions: [],
    static_addresses: 0,
    address_subscriptions: [],
    can_manage: false,
    can_subscribe: true,
    ...overrides,
  };
}

// An account on `tier` (its servers' worth on shared machines), active.
export function subscribed(tier: 1 | 2 | 3, overrides: Partial<MyBilling> = {}): MyBilling {
  return myBilling({
    max_servers: tier,
    shared_servers: tier,
    can_start: true,
    subscription: subscription({ tier }),
    can_manage: true,
    can_subscribe: false,
    ...overrides,
  });
}
