// GET /api/admin/online: who is on NovaWorld now (types/online.ts).

import { adminCall, type AdminResult } from './admin';
import { adminClient } from './client';
import type { OnlineAccount, OnlinePlayer, OnlineResponse, OnlineServer } from '../types/online';

function str(v: unknown): string {
  return typeof v === 'string' ? v : '';
}

function pickAccount(value: unknown): OnlineAccount | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || typeof v.id !== 'number' || typeof v.username !== 'string') return null;
  return { id: v.id, username: v.username, nwhandle: str(v.nwhandle) };
}

function pickServer(value: unknown): OnlineServer | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || typeof v.rid !== 'number') return null;
  return {
    rid: v.rid,
    name: str(v.name),
    mission: str(v.mission),
    game_type: str(v.game_type),
    hosted_server_id: typeof v.hosted_server_id === 'number' ? v.hosted_server_id : null,
  };
}

function pickGame(value: unknown): { slug: string; display_name: string } | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || typeof v.slug !== 'string') return null;
  return { slug: v.slug, display_name: str(v.display_name) || v.slug };
}

export function pickOnlinePlayer(value: unknown): OnlinePlayer | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null || typeof v.name !== 'string') return null;
  return {
    name: v.name,
    account: pickAccount(v.account),
    game: pickGame(v.game),
    state: v.state === 'playing' ? 'playing' : 'lobby',
    server: pickServer(v.server),
    since_ms: typeof v.since_ms === 'number' ? v.since_ms : 0,
    via: v.via === 'novalogic' ? 'novalogic' : 'opennova',
    session_id: typeof v.session_id === 'number' ? v.session_id : null,
    client: typeof v.client === 'string' && v.client !== '' ? v.client : null,
  };
}

export function fetchOnline(signal?: AbortSignal): Promise<AdminResult<OnlineResponse>> {
  return adminCall(
    () => adminClient.get<unknown>('/online', { signal }),
    (data) => {
      const v = data as Record<string, unknown> | null;
      if (typeof v !== 'object' || v === null || !Array.isArray(v.players)) return null;
      const players: OnlinePlayer[] = [];
      for (const p of v.players) {
        const player = pickOnlinePlayer(p);
        if (player) players.push(player);
      }
      return { count: typeof v.count === 'number' ? v.count : players.length, players };
    },
  );
}
