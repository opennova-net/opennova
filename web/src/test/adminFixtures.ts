// Accounts, the fleet and the billing overview for the admin tests, shaped as
// the admin routes answer them.

import { hostedServer } from './hostedFixtures';
import type { AdminBillingResponse, BillingSubscriptionRow } from '../types/billing';
import type { FleetMachine, FleetResponse, FleetServer, HostedServer, MachineDetail, RollPlan } from '../types/hostedServer';
import type { User, UserDetail, UserListEntry } from '../types/users';

export function user(id: number, overrides: Partial<User> = {}): User {
  return {
    id,
    username: `player${id}`,
    pcid: '0000ABCD',
    nwh: '1',
    nwhandle: `Player${id}`,
    account_status: 'active',
    email: `player${id}@example.com`,
    role: 'player',
    comp_servers: 0,
    comp_own_machines: 0,
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
    entitlement: {
      max_servers: 2,
      source: 'tier',
      tier: 2,
      comp_servers: 0,
      comp_own_machines: 0,
      tier_servers: 2,
      own_machines: 0,
      can_start: true,
    },
    capacity: {
      shared: { sold: 4, sellable: 24 },
      own_machines: { sold: 1, sellable: 4 },
      addresses: { sold: 0, sellable: 4 },
      oversold: false,
    },
    servers: [],
    addresses: [],
    billing: { livemode: false, stripe_customer_id: 'cus_test_1', subscriptions: [] },
    ...overrides,
  };
}

const GIB = 1024 * 1024 * 1024;
export const HOSTD_IMAGE = 'ghcr.io/opennova-net/opennova-hostd@sha256:1111111111111111111111111111111111111111111111111111111111111111';

// A fleet_machines row (its id the row's, as the routes answer it) and the
// cloud machine it stands for: on the release's images, its agent's last
// report 4 s before 2026-10-18 12:00:00 UTC (usageFixtures' NOW).
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
    agent_version: '0.0.20',
    agent_protocol: 3,
    agent_image: HOSTD_IMAGE,
    agent_log_stream: `hostd/hostd/task${id}`,
    protocol_mismatch_since: null,
    lost_held: false,
    image_id: 'ami-0aaa',
    last_report_at: '2026-10-18T11:59:56Z',
    launched_at: '2026-10-18 10:00:00',
    ready_at: '2026-10-18 10:03:00',
    ended_at: null,
    end_reason: null,
    stats: {
      cpu_percent: 22,
      memory_used_bytes: 2 * GIB,
      memory_total_bytes: 4 * GIB,
      disk_free_bytes: 30_000_000_000,
      disk_total_bytes: 40_000_000_000,
      net_in_bytes: 9_000_000_000,
      net_out_bytes: 40_000_000_000,
      net_in_bps: 300_000,
      net_out_bps: 1_200_000,
    },
    stats_at: '2026-10-18T11:59:56Z',
    release: { image_id: 'ami-0aaa', agent_image: HOSTD_IMAGE },
    outdated: false,
    address: null,
    // Server 1 runs in slot 3, server 2 starts in slot 5.
    slot_holders: [
      { slot: 3, server_id: 1, state: 'running' },
      { slot: 5, server_id: 2, state: 'booting' },
    ],
    ...overrides,
  };
}

// GET /api/admin/fleet/machines/<id>: the machine with its slot holders naming
// their servers (server 1, Ranked TDM, late in its last minute; server 2 with
// no name, before its first placement and report), its events from its making,
// a ready base tree and an expansion half fetched.
export function machineDetail(id: string, overrides: Partial<MachineDetail> = {}): MachineDetail {
  return {
    machine: machine(id, {
      slot_holders: [
        { slot: 3, server_id: 1, state: 'running', name: 'Ranked TDM', game_port: 32771, tick: { p99_ms: 31, late: 3 } },
        { slot: 5, server_id: 2, state: 'booting', name: '', game_port: null, tick: null },
      ],
    }),
    events: [
      { from: null, to: 'launching', detail: '', at: '2026-10-18 10:00:00' },
      { from: 'launching', to: 'booting', detail: 'the cloud started it', at: '2026-10-18 10:01:00' },
      { from: 'booting', to: 'booting', detail: `log stream hostd/hostd/task${id}`, at: '2026-10-18 10:02:00' },
      { from: 'booting', to: 'ready', detail: 'first report', at: '2026-10-18 10:03:00' },
    ],
    trees: [
      { tree_id: 10, state: 'ready', bytes: 1_200_000_000, total_bytes: 1_200_000_000, tries: 1, error: null, reported_at: '2026-10-18T11:59:56Z' },
      { tree_id: 12, state: 'fetching', bytes: 300_000_000, total_bytes: 600_000_000, tries: 2, error: null, reported_at: '2026-10-18T11:59:56Z' },
    ],
    ...overrides,
  };
}

// GET /api/admin/fleet/roll: machine 3 on an older image, machine 4 on the
// templates' own.
export function rollPlan(overrides: Partial<RollPlan> = {}): RollPlan {
  return {
    image_id: 'ami-0bbb',
    machines: [
      { id: '3', kind: 'shared', state: 'ready', image_id: 'ami-0aaa', servers: 2, action: 'replace', why: "its image is not the templates' (ami-0bbb)" },
      { id: '4', kind: 'own', state: 'ready', image_id: 'ami-0bbb', servers: 1, action: 'keep', why: "its image is the templates'" },
    ],
    replace: ['3'],
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
    base_tree_id: 10,
    expansion_tree_id: null,
    outdated: false,
    generation: 1,
    tick: { p99_ms: 14.2, late: 0 },
    ...overrides,
  };
}

export function fleetResponse(overrides: Partial<FleetResponse> = {}): FleetResponse {
  const servers = overrides.servers ?? [fleetServer(1), fleetServer(2, { state: 'stopped', players: null, machine_ref: null, slot: null, game_port: null })];
  const live = servers.filter((s) => s.live).length;
  return {
    enabled: true,
    cap: 26,
    live,
    shared: { live, room: 24 },
    own: { live: 0, room: 2 },
    machines: [
      machine('3'),
      machine('4', { kind: 'own', slots: 1, slots_used: 1, instance_type: 'c7a.medium', slot_holders: [{ slot: 0, server_id: 9, state: 'running' }] }),
    ],
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
