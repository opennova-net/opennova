// Stats replies for the stats tests, shaped as the stats routes answer them.

import type { HostedStats, StatsPoint } from '../types/hostedStats';

// Twelve 5-minute buckets ending at 13:00 UTC: running, a stretch not
// running (buckets 4 and 5), running again.
export function statsPoints(): StatsPoint[] {
  const start = Date.UTC(2026, 9, 14, 12, 0);
  return Array.from({ length: 12 }, (_, i) => {
    const running = i !== 4 && i !== 5;
    return {
      t: new Date(start + i * 300_000).toISOString(),
      players: running ? i : null,
      max_players: running ? 32 : null,
      running,
      cpu_percent: running ? 10 + i : null,
      egress_bytes: running ? 1_000_000 * i : null,
    };
  });
}

export function serverStats(overrides: Partial<HostedStats> = {}): HostedStats {
  return {
    server_id: 7,
    range: '1h',
    live: {
      state: 'running',
      uptime_seconds: 3 * 3600 + 5 * 60,
      players: 11,
      max_players: 32,
      mission: 'jo_one.bms',
      game_type: 'TDM',
      time_left: 600,
      scene: 'Game',
      link_state: 'up',
      cpu_percent: 21.4,
      memory_bytes: 180_000_000,
    },
    summary: {
      live_seconds: 3600,
      running_seconds: 3000,
      availability: 3000 / 3600,
      restarts: 1,
      peak_players: 11,
      avg_players: 5.25,
      player_hours: 4.5,
      unique_players: 17,
      missions: 3,
      egress_bytes: 1_250_000_000,
      approximate: ['unique_players'],
    },
    series: { step_seconds: 300, points: statsPoints() },
    ...overrides,
  };
}
