// Hosted server stats, as the NovaWorld service's stats routes answer them
// (snake_case; apps/novaworld_server/hosted/stats_routes.cpp): one server's,
// the account's across its servers, or the fleet's. The service samples each
// live server every minute and buckets the series by the range (1h raw, 24h
// by 5 minutes, 7d hourly, 30d by 4 hours). Times are ISO 8601 UTC.

import type { HostedState } from './hostedServer';

export type StatsRange = '1h' | '24h' | '7d' | '30d';

// The server as its last sample saw it; null for an aggregate, and for a
// server that is not live.
export interface LiveStats {
  state: HostedState;
  uptime_seconds: number;
  players: number;
  max_players: number;
  mission: string;
  game_type: string;
  // Seconds left in the round, null when the server did not say.
  time_left: number | null;
  // 'Game' (a match), 'Menus' (between matches), or '' before a poll.
  scene: string;
  // The service's console link: 'connecting', 'up', 'down' or 'stopped'.
  link_state: string;
  // The machine's last report for the server's slot; null before one.
  cpu_percent: number | null;
  memory_bytes: number | null;
}

// The range's totals.
export interface StatsSummary {
  // How long the server was live (any state but ended), and running, in it.
  live_seconds: number;
  running_seconds: number;
  // running / live, 0 to 1; null when it was never live.
  availability: number | null;
  restarts: number;
  peak_players: number;
  avg_players: number;
  player_hours: number;
  unique_players: number;
  missions: number;
  // Bytes sent to players.
  egress_bytes: number;
  // The fields above that are estimates (summed across hours that may count
  // the same player twice, say), by name.
  approximate: string[];
}

// One bucket of the series. A bucket in which the server was not running has
// running false (its values then count for nothing).
export interface StatsPoint {
  t: string;
  players: number | null;
  max_players: number | null;
  running: boolean;
  cpu_percent: number | null;
  egress_bytes: number | null;
}

export interface StatsSeries {
  step_seconds: number;
  points: StatsPoint[];
}

export interface HostedStats {
  // The server's id; absent for an aggregate.
  server_id?: number;
  range: StatsRange;
  live: LiveStats | null;
  summary: StatsSummary;
  series: StatsSeries;
}
