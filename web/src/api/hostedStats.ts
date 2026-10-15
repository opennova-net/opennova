// Hosted server stats (apps/novaworld_server/hosted/stats_routes.cpp): one of
// the account's servers (GET /api/me/servers/<id>/stats), the account across
// its servers (GET /api/me/servers/stats), and the admin's per server (GET
// /api/admin/hosted/<id>/stats) and fleet (GET /api/admin/hosted/stats), each
// for a range (?range=1h|24h|7d|30d).
//
// Each call answers a StatsResult instead of throwing, with the server's
// `error` code mapped to a message; the server's own text is never shown.

import { isAxiosError, type AxiosInstance } from 'axios';
import { adminClient, apiClient } from './client';
import { formatRetryAfter, parseRetryAfter } from './session';
import type { HostedState } from '../types/hostedServer';
import type { HostedStats, LiveStats, StatsPoint, StatsRange, StatsSummary } from '../types/hostedStats';

export type StatsErrorCode =
  | 'unauthorized'
  | 'forbidden'
  | 'not_found'
  | 'invalid_range'
  | 'rate_limited'
  | 'db_error'
  | 'network'
  | 'unknown';

export interface StatsError {
  code: StatsErrorCode;
  status: number | null;
  message: string;
  retryAfterSeconds?: number;
}

export type StatsResult<T> = { ok: true; value: T } | { ok: false; error: StatsError };

const KNOWN_CODES: ReadonlySet<string> = new Set<StatsErrorCode>([
  'unauthorized',
  'forbidden',
  'not_found',
  'invalid_range',
  'rate_limited',
  'db_error',
]);

export const STATS_RANGES: readonly StatsRange[] = ['1h', '24h', '7d', '30d'];

function messageFor(code: StatsErrorCode, retryAfterSeconds?: number): string {
  switch (code) {
    case 'unauthorized':
      return 'Your session has ended. Log in again to see the stats.';
    case 'forbidden':
      return 'This account is not an admin.';
    case 'not_found':
      return 'That hosted server no longer exists.';
    case 'invalid_range':
      return 'The server could not read the request. Reload the page and try again.';
    case 'rate_limited':
      return retryAfterSeconds !== undefined
        ? `Too many requests for stats. Try again in ${formatRetryAfter(retryAfterSeconds)}.`
        : 'Too many requests for stats. Try again in a little while.';
    case 'db_error':
      return 'The server hit a database error. Try again shortly.';
    case 'network':
      return 'Could not reach the server. Check your connection and try again.';
    case 'unknown':
      return 'Something went wrong. Try again shortly.';
  }
}

export function describeStatsError(err: unknown): StatsError {
  if (isAxiosError(err)) {
    const response = err.response;
    if (!response) return { code: 'network', status: null, message: messageFor('network') };
    const data = response.data as { error?: unknown } | undefined;
    const serverCode = typeof data?.error === 'string' ? data.error : '';
    const code: StatsErrorCode = KNOWN_CODES.has(serverCode)
      ? (serverCode as StatsErrorCode)
      : response.status === 429
        ? 'rate_limited'
        : response.status === 401
          ? 'unauthorized'
          : 'unknown';
    const retryAfterSeconds =
      code === 'rate_limited' ? parseRetryAfter(response.headers?.['retry-after']) : undefined;
    return {
      code,
      status: response.status,
      message:
        code === 'unknown' ? `${messageFor('unknown')} (HTTP ${response.status})` : messageFor(code, retryAfterSeconds),
      ...(retryAfterSeconds !== undefined ? { retryAfterSeconds } : {}),
    };
  }
  return { code: 'unknown', status: null, message: messageFor('unknown') };
}

const isNumber = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v);
const isCount = (v: unknown): v is number => Number.isInteger(v) && (v as number) >= 0;
const isNumberOrNull = (v: unknown): v is number | null => v === null || isNumber(v);

function parseLive(value: unknown): LiveStats | null | undefined {
  if (value === null || value === undefined) return null;
  const v = value as Record<string, unknown>;
  if (typeof v !== 'object') return undefined;
  if (typeof v.state !== 'string' || !isNumber(v.uptime_seconds) || !isCount(v.players) || !isCount(v.max_players)) {
    return undefined;
  }
  if (!isNumberOrNull(v.cpu_percent ?? null) || !isNumberOrNull(v.memory_bytes ?? null)) return undefined;
  return {
    state: v.state as HostedState,
    uptime_seconds: v.uptime_seconds,
    players: v.players,
    max_players: v.max_players,
    mission: typeof v.mission === 'string' ? v.mission : '',
    game_type: typeof v.game_type === 'string' ? v.game_type : '',
    time_left: isNumber(v.time_left) ? v.time_left : null,
    scene: typeof v.scene === 'string' ? v.scene : '',
    link_state: typeof v.link_state === 'string' ? v.link_state : '',
    cpu_percent: (v.cpu_percent ?? null) as number | null,
    memory_bytes: (v.memory_bytes ?? null) as number | null,
  };
}

function parseSummary(value: unknown): StatsSummary | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null) return null;
  const numbers = [
    'live_seconds',
    'running_seconds',
    'restarts',
    'peak_players',
    'avg_players',
    'player_hours',
    'unique_players',
    'missions',
    'egress_bytes',
  ] as const;
  if (!numbers.every((k) => isNumber(v[k]) && (v[k] as number) >= 0)) return null;
  if (!isNumberOrNull(v.availability ?? null)) return null;
  const approximate = Array.isArray(v.approximate) ? v.approximate.filter((a): a is string => typeof a === 'string') : [];
  return {
    live_seconds: v.live_seconds as number,
    running_seconds: v.running_seconds as number,
    availability: (v.availability ?? null) as number | null,
    restarts: v.restarts as number,
    peak_players: v.peak_players as number,
    avg_players: v.avg_players as number,
    player_hours: v.player_hours as number,
    unique_players: v.unique_players as number,
    missions: v.missions as number,
    egress_bytes: v.egress_bytes as number,
    approximate,
  };
}

function parsePoint(value: unknown): StatsPoint | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null) return null;
  if (typeof v.t !== 'string' || Number.isNaN(Date.parse(v.t)) || typeof v.running !== 'boolean') return null;
  const opt = (k: string): number | null => (isNumber(v[k]) ? (v[k] as number) : null);
  return {
    t: v.t,
    players: opt('players'),
    max_players: opt('max_players'),
    running: v.running,
    cpu_percent: opt('cpu_percent'),
    egress_bytes: opt('egress_bytes'),
  };
}

// A stats reply, or null when its summary or any point is malformed.
export function parseStats(value: unknown): HostedStats | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null) return null;
  if (!STATS_RANGES.includes(v.range as StatsRange)) return null;
  const live = parseLive(v.live);
  const summary = parseSummary(v.summary);
  const seriesIn = v.series as { step_seconds?: unknown; points?: unknown } | null;
  if (live === undefined || summary === null || typeof seriesIn !== 'object' || seriesIn === null) return null;
  if (!isNumber(seriesIn.step_seconds) || seriesIn.step_seconds <= 0 || !Array.isArray(seriesIn.points)) return null;
  const points = seriesIn.points.map(parsePoint);
  if (points.some((p) => p === null)) return null;
  return {
    ...(isCount(v.server_id) ? { server_id: v.server_id } : {}),
    range: v.range as StatsRange,
    live,
    summary,
    series: { step_seconds: seriesIn.step_seconds, points: points as StatsPoint[] },
  };
}

async function getStats(client: AxiosInstance, path: string, range: StatsRange, signal?: AbortSignal): Promise<StatsResult<HostedStats>> {
  try {
    const res = await client.get<unknown>(path, { params: { range }, signal });
    const stats = parseStats(res.data);
    if (!stats) return { ok: false, error: { code: 'unknown', status: res.status, message: messageFor('unknown') } };
    return { ok: true, value: stats };
  } catch (err) {
    return { ok: false, error: describeStatsError(err) };
  }
}

// One server's stats: the owner's through their routes, the admin's through
// the fleet's.
export function fetchServerStats(
  scope: 'owner' | 'admin',
  id: number,
  range: StatsRange,
  signal?: AbortSignal,
): Promise<StatsResult<HostedStats>> {
  return scope === 'admin'
    ? getStats(adminClient, `/hosted/${id}/stats`, range, signal)
    : getStats(apiClient, `/me/servers/${id}/stats`, range, signal);
}

// The account's servers together.
export function fetchAccountStats(range: StatsRange, signal?: AbortSignal): Promise<StatsResult<HostedStats>> {
  return getStats(apiClient, '/me/servers/stats', range, signal);
}

// The whole fleet (admin).
export function fetchFleetStats(range: StatsRange, signal?: AbortSignal): Promise<StatsResult<HostedStats>> {
  return getStats(adminClient, '/hosted/stats', range, signal);
}
