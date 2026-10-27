// GET /api/admin/online's rows for the tests: a player of ours, signed in and
// playing on a hosted server unless the overrides say otherwise.

import type { OnlinePlayer, OnlineResponse } from '../types/online';

let nextId = 1;

export function onlinePlayer(name: string, overrides: Partial<OnlinePlayer> = {}): OnlinePlayer {
  const id = nextId++;
  return {
    name,
    account: { id, username: name.toLowerCase(), nwhandle: name },
    game: { slug: 'jop_2_consumer', display_name: 'Joint Operations' },
    state: 'playing',
    server: { rid: 167772161, name: 'Night Raid', mission: 'River Patrol', game_type: 'TDM', hosted_server_id: 1 },
    since_ms: Date.parse('2026-10-27T12:00:00Z'),
    via: 'opennova',
    session_id: id,
    client: 'Jointops.exe',
    ...overrides,
  };
}

export function onlineResponse(players: OnlinePlayer[]): OnlineResponse {
  return { count: players.length, players };
}
