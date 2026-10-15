// Data sets, settings, servers and entitlements for the hosted server tests,
// shaped as GET /api/me/servers answers them. DFX2's limits differ from JO's
// here on purpose, so a test can tell whose ranges a form applies.

import type {
  HostedDataSet,
  HostedServer,
  HostedServersResponse,
  HostedSettings,
  HostingEntitlement,
} from '../types/hostedServer';
import type { MyServer } from '../types/myServers';

export const JO: HostedDataSet = {
  game: 'jop_2_consumer',
  display_name: 'Joint Operations',
  expansions: [
    { code: '', name: 'Joint Operations (base game)' },
    { code: 'jox01', name: 'Escalation (jox01)' },
  ],
  default_missions: ['jo_one.bms', 'jo_two.bms'],
  limits: {
    max_players: { min: 2, max: 64 },
    time_limit: { min: 1, max: 300 },
    kill_limit: { min: 1, max: 500 },
    max_score: { min: 1, max: 500 },
    respawn: { min: 0, max: 300 },
    start_delay: { min: 0, max: 300 },
  },
};

export const DFX2: HostedDataSet = {
  game: 'dfx2_consumer',
  display_name: 'Delta Force Xtreme 2',
  expansions: [{ code: '', name: 'Delta Force Xtreme 2 (base game)' }],
  default_missions: ['dfx_one.bms'],
  limits: {
    max_players: { min: 2, max: 50 },
    time_limit: { min: 1, max: 120 },
    kill_limit: { min: 1, max: 250 },
    max_score: { min: 1, max: 250 },
    respawn: { min: 0, max: 60 },
    start_delay: { min: 0, max: 60 },
  },
};

export function settings(overrides: Partial<HostedSettings> = {}): HostedSettings {
  return {
    game: JO.game,
    game_name: 'Test Server',
    max_players: 32,
    game_password: '',
    time_limit: 30,
    kill_limit: 50,
    max_score: 5,
    respawn: 5,
    start_delay: 0,
    team_ff: false,
    expansion: '',
    missions: ['jo_one.bms'],
    ...overrides,
  };
}

// A hosted server, running unless the overrides say otherwise.
export function hostedServer(id: number, overrides: Partial<HostedServer> = {}): HostedServer {
  const state = overrides.state ?? 'running';
  const live = state !== 'stopped' && state !== 'failed';
  return {
    id,
    state,
    live,
    own_machine: false,
    settings: settings({ game_name: `Server ${id}` }),
    game_display_name: 'Joint Operations',
    expansion_display_name: 'Joint Operations (base game)',
    restarts: 0,
    failure_reason: null,
    created_at: '2026-10-14 12:00:00',
    updated_at: '2026-10-14 12:05:00',
    running_at: state === 'running' ? '2026-10-14 12:02:00' : null,
    ended_at: live ? null : '2026-10-14 13:00:00',
    listing: null,
    ...overrides,
  };
}

export function entitlement(overrides: Partial<HostingEntitlement> = {}): HostingEntitlement {
  return {
    max_servers: 3,
    used: 0,
    own_machines: 0,
    own_used: 0,
    can_start: true,
    source: 'tier',
    ...overrides,
  };
}

// GET /api/me/servers, its entitlement's `used` counted from the live servers
// unless the overrides name it.
export function serversResponse(
  servers: HostedServer[],
  overrides: Partial<Omit<HostedServersResponse, 'entitlement'>> & { entitlement?: Partial<HostingEntitlement> } = {},
): HostedServersResponse {
  const { entitlement: e, ...rest } = overrides;
  return {
    enabled: true,
    full: false,
    data_sets: [JO, DFX2],
    servers,
    ...rest,
    entitlement: entitlement({ used: servers.filter((s) => s.live).length, ...e }),
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
