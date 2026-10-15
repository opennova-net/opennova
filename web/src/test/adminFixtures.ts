// Accounts, the fleet and the billing overview for the admin tests, shaped as
// the admin routes answer them.

import { hostedServer } from './hostedFixtures';
import type { AdminBillingResponse, BillingSubscriptionRow } from '../types/billing';
import type { FleetMachine, FleetResponse, FleetServer, HostedServer } from '../types/hostedServer';
import type { User, UserDetail, UserListEntry } from '../types/users';

export function user(id: number, overrides: Partial<User> = {}): User {
  return {
    id,
    username: `player${id}`,
    pcid: '0000ABCD',
    nwh: '1',
    nwhandle: `Player${id}`,
    account_status: 'active',
    role: 'player',
    comp_servers: 0,
    max_servers: 0,
    ...overrides,
  };
}

export function userEntry(id: number, overrides: Partial<UserListEntry> = {}): UserListEntry {
  return {
    ...user(id),
    entitlement: { max_servers: 0, source: 'none', tier: null },
    live_servers: 0,
    subscription_status: null,
    ...overrides,
  };
}

export function userDetail(id: number, overrides: Partial<UserDetail> = {}): UserDetail {
  return {
    user: user(id, { max_servers: 2 }),
    entitlement: { max_servers: 2, source: 'tier', tier: 2, comp_servers: 0, tier_servers: 2, own_machines: 0, can_start: true },
    servers: [],
    billing: { livemode: false, stripe_customer_id: 'cus_test_1', subscriptions: [] },
    ...overrides,
  };
}

// A fleet_machines row (its id the row's, as the routes answer it) and the
// cloud machine it stands for.
export function machine(id: string, overrides: Partial<FleetMachine> = {}): FleetMachine {
  return {
    id,
    cloud_id: `i-0abc00000000000${id}`,
    kind: 'shared',
    state: 'ready',
    instance_type: 't3a.large',
    slots: 12,
    slots_used: 2,
    public_address: '203.0.113.7',
    egress_bytes_month: 12_500_000_000,
    ...overrides,
  };
}

export function fleetServer(id: number, overrides: Partial<FleetServer> & Partial<HostedServer> = {}): FleetServer {
  return {
    ...hostedServer(id, overrides),
    owner: { id: 100 + id, username: `owner${id}`, tier: 2 },
    machine_ref: '3',
    slot: 3,
    game_port: 32771,
    players: 5,
    ...overrides,
  };
}

export function fleetResponse(overrides: Partial<FleetResponse> = {}): FleetResponse {
  const servers = overrides.servers ?? [fleetServer(1), fleetServer(2, { state: 'stopped', players: null, machine_ref: null, slot: null, game_port: null })];
  return {
    enabled: true,
    cap: 24,
    live: servers.filter((s) => s.live).length,
    machines: [machine('3'), machine('4', { kind: 'own', slots: 1, slots_used: 1, instance_type: 'c7a.medium' })],
    servers,
    ...overrides,
  };
}

export function subscriptionRow(userId: number, overrides: Partial<BillingSubscriptionRow> = {}): BillingSubscriptionRow {
  return {
    user_id: userId,
    username: `player${userId}`,
    stripe_customer_id: `cus_${userId}`,
    stripe_subscription_id: `sub_${userId}`,
    item: 'tier',
    tier: 1,
    quantity: 1,
    status: 'active',
    entitled: true,
    current_period_end: '2026-11-14T12:00:00Z',
    cancel_at_period_end: false,
    ...overrides,
  };
}

export function adminBilling(overrides: Partial<AdminBillingResponse> = {}): AdminBillingResponse {
  return {
    enabled: true,
    limits_hold: false,
    limits_hold_reason: null,
    limits_hold_owners: 0,
    limits_hold_servers: 0,
    mode: 'test',
    prices_ok: true,
    last_reconcile_at: '2026-10-14T06:00:00Z',
    last_reconcile_error: null,
    pending_events: 0,
    subscriptions: [
      subscriptionRow(1, { tier: 1 }),
      subscriptionRow(2, { tier: 2 }),
      subscriptionRow(3, { tier: 2, status: 'past_due' }),
      subscriptionRow(4, { tier: 3, cancel_at_period_end: true }),
      subscriptionRow(5, { tier: 1, status: 'canceled', entitled: false }),
      subscriptionRow(4, { item: 'own_machine', tier: null, quantity: 2, stripe_subscription_id: 'sub_4_own' }),
    ],
    duplicates: [],
    unknown_prices: [],
    orphans: [],
    recent_events: [{ event_id: 'evt_1', type: 'invoice.paid', received_at: '2026-10-14T12:00:00Z', outcome: 'processed' }],
    ...overrides,
  };
}
