// Hosted server stats, as the NovaWorld service's stats routes answer them
// (snake_case; apps/novaworld_server/hosted/stats_routes.cpp): one server's,
// the account's across its servers, or the fleet's. The service samples each
// live server every minute and buckets the series by the range (1h raw, 24h
// by 5 minutes, 7d hourly, 30d by 4 hours). Times are ISO 8601 UTC.

import type { HostedState, MachineKind, MachineState } from './hostedServer';

export type StatsRange = '1h' | '24h' | '7d' | '30d';

// The server as its last sample saw it; null for an aggregate, and for a
// server that is not live.
export interface LiveStats {
  state: HostedState;
  // Seconds since it last reached running; null while it is not running.
  uptime_seconds: number | null;
  // From its listing; null while it is not listed.
  players: number | null;
  max_players: number | null;
  mission: string;
  game_type: string;
  // Seconds left in the round, null when the server did not say.
  time_left: number | null;
  // 'game' (a match), 'menus' (between matches), 'unknown', or '' before a poll.
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
  // null when the range has no sample of it.
  avg_players: number | null;
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

// The admin's usage view (GET /api/admin/hosted/usage): what every live
// hosted server uses of its slot, from the service's samples (one a minute)
// of what each machine's agent reports: its container's memory and CPU, and
// the bytes its slot sends to players. Nothing here is sampled anew.

// What a set of servers uses: the fleet's, or the servers on one machine.
export interface UsageTotals {
  // The servers whose newest sample (at most 3 minutes old) carries their
  // agent's CPU and memory; the memory and CPU figures cover these alone.
  reporting: number;
  players: number;
  // Their memory, and their slots' limits added up.
  memory_bytes: number;
  memory_limit_bytes: number;
  // Their CPU use and limits, in CPUs.
  cpus_used: number;
  cpus_limit: number;
  // Bytes a second sent to players over each server's newest sampled minute,
  // added up.
  egress_bytes_per_second: number;
  // Bytes sent to players in the window (the last hour).
  egress_bytes_window: number;
}

export interface FleetUsageTotals extends UsageTotals {
  servers_live: number;
  machines_live: number;
  // The live machines' slots, and how many hold a live server.
  slots: number;
  slots_used: number;
}

// A live machine and what the servers on it use.
export interface MachineUsage extends UsageTotals {
  id: string;
  kind: MachineKind;
  state: MachineState | (string & {});
  instance_type: string;
  slots: number;
  slots_used: number;
}

// A server's samples in the window, summed up; the CPU and memory are null
// when none of them carried the agent's numbers.
export interface ServerUsageWindow {
  minutes: number;
  cpu_percent_mean: number | null;
  cpu_percent_peak: number | null;
  memory_bytes_peak: number | null;
  egress_bytes: number;
}

// A live server: its newest sample's figures (each null without a sample in
// the last 3 minutes, or when it did not carry them) and its window.
export interface ServerUsage {
  id: number;
  name: string;
  state: HostedState;
  own_machine: boolean;
  owner: { id: number; username: string } | null;
  // The machine it is placed on and its slot there; null before placement.
  machine_ref: string | null;
  slot: number | null;
  // Seconds since it last reached running; null while it is not running.
  uptime_seconds: number | null;
  sample_at: string | null;
  players: number | null;
  max_players: number | null;
  // Of its slot's CPUs, 0 to 100.
  cpu_percent: number | null;
  memory_bytes: number | null;
  egress_bytes_per_second: number | null;
  window: ServerUsageWindow;
}

export interface FleetUsage {
  // When the service answered (ISO 8601 UTC).
  at: string;
  window_seconds: number;
  // Each container's limits.
  slot_memory_bytes: number;
  slot_cpus: number;
  totals: FleetUsageTotals;
  // The live machines by id.
  machines: MachineUsage[];
  // The live servers by machine, then slot; the ones placed nowhere last.
  servers: ServerUsage[];
}
