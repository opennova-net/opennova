// Hosted server stats (apps/novaworld_web/hosted/stats_routes.cpp): one of
// the account's servers (GET /api/me/servers/<id>/stats), the account across
// its servers (GET /api/me/servers/stats), and the admin's per server (GET
// /api/admin/hosted/<id>/stats) and fleet (GET /api/admin/hosted/stats), each
// for a range (?range=1h|24h|7d|30d); and the admin's usage view of every
// live server (GET /api/admin/hosted/usage).
//
// Each call answers a StatsResult instead of throwing, with the server's
// `error` code mapped to a message; the server's own text is never shown.

import { isAxiosError, type AxiosInstance } from 'axios';
import { adminClient, apiClient } from './client';
import { formatRetryAfter, parseRetryAfter } from './session';
import type { HostedState, MachineKind } from '../types/hostedServer';
import type {
  FleetUsage,
  FleetUsageTotals,
  HostedStats,
  LiveStats,
  MachineUsage,
  ServerUsage,
  StatsPoint,
  StatsRange,
  StatsSummary,
  UsageTotals,
} from '../types/hostedStats';

export type StatsErrorCode =
  | 'unauthorized'
  | 'forbidden'
  | 'not_found'
  | 'invalid_field'
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
  'invalid_field',
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
    case 'invalid_field':
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
  const countOrNull = (x: unknown) => x === null || x === undefined || isCount(x);
  if (typeof v.state !== 'string' || !isNumberOrNull(v.uptime_seconds ?? null) || !countOrNull(v.players) || !countOrNull(v.max_players)) {
    return undefined;
  }
  if (!isNumberOrNull(v.cpu_percent ?? null) || !isNumberOrNull(v.memory_bytes ?? null)) return undefined;
  return {
    state: v.state as HostedState,
    uptime_seconds: (v.uptime_seconds ?? null) as number | null,
    players: (v.players ?? null) as number | null,
    max_players: (v.max_players ?? null) as number | null,
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
    'player_hours',
    'unique_players',
    'missions',
    'egress_bytes',
  ] as const;
  if (!numbers.every((k) => isNumber(v[k]) && (v[k] as number) >= 0)) return null;
  if (!isNumberOrNull(v.availability ?? null) || !isNumberOrNull(v.avg_players ?? null)) return null;
  const approximate = Array.isArray(v.approximate) ? v.approximate.filter((a): a is string => typeof a === 'string') : [];
  return {
    live_seconds: v.live_seconds as number,
    running_seconds: v.running_seconds as number,
    availability: (v.availability ?? null) as number | null,
    restarts: v.restarts as number,
    peak_players: v.peak_players as number,
    avg_players: (v.avg_players ?? null) as number | null,
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

const USAGE_FIELDS = [
  'reporting',
  'players',
  'memory_bytes',
  'memory_limit_bytes',
  'cpus_used',
  'cpus_limit',
  'egress_bytes_per_second',
  'egress_bytes_window',
] as const;

// The usage fields of the fleet's totals or of a machine; null when one is
// missing or negative.
function parseUsageTotals(v: Record<string, unknown>): UsageTotals | null {
  if (!USAGE_FIELDS.every((k) => isNumber(v[k]) && (v[k] as number) >= 0)) return null;
  return Object.fromEntries(USAGE_FIELDS.map((k) => [k, v[k] as number])) as unknown as UsageTotals;
}

function parseMachineUsage(value: unknown): MachineUsage | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null) return null;
  if ((typeof v.id !== 'number' && typeof v.id !== 'string') || (v.kind !== 'shared' && v.kind !== 'own')) return null;
  if (typeof v.state !== 'string' || !isCount(v.slots) || !isCount(v.slots_used)) return null;
  const usage = parseUsageTotals(v);
  if (!usage) return null;
  return {
    ...usage,
    id: String(v.id),
    kind: v.kind as MachineKind,
    state: v.state,
    instance_type: typeof v.instance_type === 'string' ? v.instance_type : '',
    slots: v.slots,
    slots_used: v.slots_used,
  };
}

function parseServerUsage(value: unknown): ServerUsage | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null) return null;
  if (!isCount(v.id) || typeof v.name !== 'string' || typeof v.state !== 'string') return null;
  const w = v.window as Record<string, unknown> | null;
  if (typeof w !== 'object' || w === null || !isCount(w.minutes) || !isNumber(w.egress_bytes)) return null;
  const opt = (x: unknown): number | null => (isNumber(x) ? x : null);
  const owner = v.owner as Record<string, unknown> | null | undefined;
  return {
    id: v.id,
    name: v.name,
    state: v.state as HostedState,
    own_machine: v.own_machine === true,
    owner:
      owner && typeof owner.id === 'number' && typeof owner.username === 'string'
        ? { id: owner.id, username: owner.username }
        : null,
    machine_ref: typeof v.machine_ref === 'number' || typeof v.machine_ref === 'string' ? String(v.machine_ref) : null,
    slot: isCount(v.slot) ? v.slot : null,
    uptime_seconds: opt(v.uptime_seconds),
    sample_at: typeof v.sample_at === 'string' ? v.sample_at : null,
    players: isCount(v.players) ? v.players : null,
    max_players: isCount(v.max_players) ? v.max_players : null,
    cpu_percent: opt(v.cpu_percent),
    memory_bytes: opt(v.memory_bytes),
    egress_bytes_per_second: opt(v.egress_bytes_per_second),
    window: {
      minutes: w.minutes,
      cpu_percent_mean: opt(w.cpu_percent_mean),
      cpu_percent_peak: opt(w.cpu_percent_peak),
      memory_bytes_peak: opt(w.memory_bytes_peak),
      egress_bytes: w.egress_bytes,
    },
  };
}

// A usage reply, or null when its totals, a machine or a server is malformed.
export function parseFleetUsage(value: unknown): FleetUsage | null {
  const v = value as Record<string, unknown> | null;
  if (typeof v !== 'object' || v === null) return null;
  if (typeof v.at !== 'string' || !isNumber(v.window_seconds) || v.window_seconds <= 0) return null;
  if (!isNumber(v.slot_memory_bytes) || !isNumber(v.slot_cpus)) return null;
  const t = v.totals as Record<string, unknown> | null;
  if (typeof t !== 'object' || t === null) return null;
  const usage = parseUsageTotals(t);
  const counts = ['servers_live', 'machines_live', 'slots', 'slots_used'] as const;
  if (!usage || !counts.every((k) => isCount(t[k]))) return null;
  if (!Array.isArray(v.machines) || !Array.isArray(v.servers)) return null;
  const machines = v.machines.map(parseMachineUsage);
  const servers = v.servers.map(parseServerUsage);
  if (machines.some((m) => m === null) || servers.some((s) => s === null)) return null;
  const totals: FleetUsageTotals = {
    ...usage,
    servers_live: t.servers_live as number,
    machines_live: t.machines_live as number,
    slots: t.slots as number,
    slots_used: t.slots_used as number,
  };
  return {
    at: v.at,
    window_seconds: v.window_seconds,
    slot_memory_bytes: v.slot_memory_bytes,
    slot_cpus: v.slot_cpus,
    totals,
    machines: machines as MachineUsage[],
    servers: servers as ServerUsage[],
  };
}

// What every live hosted server uses of its slot, with each machine's and
// the fleet's totals (admin).
export async function fetchFleetUsage(signal?: AbortSignal): Promise<StatsResult<FleetUsage>> {
  try {
    const res = await adminClient.get<unknown>('/hosted/usage', { signal });
    const usage = parseFleetUsage(res.data);
    if (!usage) return { ok: false, error: { code: 'unknown', status: res.status, message: messageFor('unknown') } };
    return { ok: true, value: usage };
  } catch (err) {
    return { ok: false, error: describeStatsError(err) };
  }
}
