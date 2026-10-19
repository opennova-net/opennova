// Data sets, settings, servers and entitlements for the hosted server tests,
// shaped as GET /api/me/servers answers them. DFX2's limits differ from JO's
// here on purpose, so a test can tell whose ranges a form applies; JO offers
// Escalation (id 2), whose tree adds a mission of its own.

import type {
  HostedDataSet,
  HostedServer,
  HostedServersResponse,
  HostedSettings,
  HostingEntitlement,
} from '../types/hostedServer';
import type { MyServer } from '../types/myServers';

export const JO: HostedDataSet = {
  id: 1,
  display_name: 'Joint Operations',
  title: 'jo',
  version: '1.7.5.5',
  missions: [
    { file: 'jo_one.bms', title: 'River Patrol' },
    { file: 'jo_two.bms', title: '' },
  ],
  limits: {
    max_players: { min: 2, max: 64 },
    time_limit: { min: 1, max: 300 },
    kill_limit: { min: 1, max: 500 },
    max_score: { min: 1, max: 500 },
    respawn: { min: 0, max: 300 },
    start_delay: { min: 0, max: 300 },
  },
  expansions: [
    {
      id: 2,
      code: 'jox01',
      display_name: 'Escalation',
      version: '1.7.5.5',
      missions: [{ file: 'jox_one.bms', title: 'Escalation Point' }],
    },
  ],
};

export const DFX2: HostedDataSet = {
  id: 3,
  display_name: 'Delta Force Xtreme 2',
  title: 'dfx2',
  version: '1.0.0.1',
  missions: [{ file: 'dfx_one.bms', title: 'Dust' }],
  limits: {
    max_players: { min: 2, max: 50 },
    time_limit: { min: 1, max: 120 },
    kill_limit: { min: 1, max: 250 },
    max_score: { min: 1, max: 250 },
    respawn: { min: 0, max: 60 },
    start_delay: { min: 0, max: 60 },
  },
  expansions: [],
};

export function settings(overrides: Partial<HostedSettings> = {}): HostedSettings {
  return {
    data_set_id: JO.id,
    expansion_id: null,
    game_name: 'Test Server',
    max_players: 32,
    game_password: '',
    time_limit: 30,
    kill_limit: 50,
    max_score: 5,
    respawn: 5,
    start_delay: 0,
    team_ff: false,
    missions: ['jo_one.bms'],
    ...overrides,
  };
}

// A hosted server, running JO 1.7.5.5 unless the overrides say otherwise.
export function hostedServer(id: number, overrides: Partial<HostedServer> = {}): HostedServer {
  const state = overrides.state ?? 'running';
  const live = state !== 'stopped' && state !== 'failed';
  return {
    id,
    state,
    live,
    own_machine: false,
    address: null,
    settings: settings({ game_name: `Server ${id}` }),
    game_display_name: 'Joint Operations',
    expansion_display_name: '',
    data_set: { id: JO.id, display_name: 'Joint Operations' },
    expansion: null,
    data: { base: { tree_id: 10, version: '1.7.5.5' }, expansion: null },
    update_available: false,
    update_requested: false,
    restarts: 0,
    failure_reason: null,
    created_at: '2026-10-14 12:00:00',
    updated_at: '2026-10-14 12:05:00',
    running_at: state === 'running' ? '2026-10-14 12:02:00' : null,
    ended_at: live ? null : '2026-10-14 13:00:00',
    listing: null,
    phase: { name: state },
    stopped_because: live ? null : { cause: 'stop', exit_code: 0, oom: false, line: '' },
    ...overrides,
  };
}

// Tier 3's three servers on shared machines unless the overrides say
// otherwise; the servers on shared machines are the ones max_servers holds
// past the dedicated machines, unless named.
export function entitlement(overrides: Partial<HostingEntitlement> = {}): HostingEntitlement {
  const max = overrides.max_servers ?? 3;
  const own = overrides.own_machines ?? 0;
  return {
    max_servers: max,
    used: 0,
    shared_servers: Math.max(0, max - own),
    shared_used: 0,
    own_machines: own,
    own_used: 0,
    can_start: true,
    source: 'tier',
    tier: 3,
    granted: false,
    ...overrides,
  };
}

// GET /api/me/servers, its entitlement's `used`, `shared_used` and `own_used`
// counted from the live servers unless the overrides name them.
export function serversResponse(
  servers: HostedServer[],
  overrides: Partial<Omit<HostedServersResponse, 'entitlement'>> & { entitlement?: Partial<HostingEntitlement> } = {},
): HostedServersResponse {
  const { entitlement: e, ...rest } = overrides;
  const live = servers.filter((s) => s.live);
  const ownUsed = live.filter((s) => s.own_machine).length;
  return {
    enabled: true,
    full: false,
    data_sets: [JO, DFX2],
    servers,
    ...rest,
    entitlement: entitlement({ used: live.length, shared_used: live.length - ownUsed, own_used: ownUsed, ...e }),
  };
}

// One of the account's listed servers (GET /api/me/hosts).
export function listedServer(rid: number, overrides: Partial<MyServer> = {}): MyServer {
  return {
    rid,
    gsid: `gsid-${rid}`,
    game: 'jop_2_consumer',
    server_name: `Listed ${rid}`,
    mission_name: 'jo_one.bms',
    game_type: 'TDM',
    players: 2,
    max_players: 32,
    host_ip: '203.0.113.7',
    host_port: 32769,
    peer_ip: '203.0.113.7',
    created_at: '2026-10-14 12:00:00',
    updated_at: '2026-10-14 12:05:00',
    msg: '',
    roster: [],
    hosted_server_id: null,
    ...overrides,
  };
}
