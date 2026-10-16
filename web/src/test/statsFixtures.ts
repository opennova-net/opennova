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
      scene: 'game',
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

const MiB = 1024 * 1024;

// A usage reply (GET /api/admin/hosted/usage) as the service answers it: a
// shared machine with two servers (one running and reporting, one booting),
// an own machine whose server's newest sample is too old, and a server placed
// nowhere yet.
export function fleetUsageReply(): Record<string, unknown> {
  const usage = (over: Record<string, unknown> = {}) => ({
    reporting: 0,
    players: 0,
    memory_bytes: 0,
    memory_limit_bytes: 0,
    cpus_used: 0,
    cpus_limit: 0,
    egress_bytes_per_second: 0,
    egress_bytes_window: 0,
    ...over,
  });
  const window = (over: Record<string, unknown> = {}) => ({
    minutes: 0,
    cpu_percent_mean: null,
    cpu_percent_peak: null,
    memory_bytes_peak: null,
    egress_bytes: 0,
    ...over,
  });
  const none = { sample_at: null, players: null, max_players: null, cpu_percent: null, memory_bytes: null, egress_bytes_per_second: null };
  return {
    at: '2026-10-16T09:41:30Z',
    window_seconds: 3600,
    slot_memory_bytes: 512 * MiB,
    slot_cpus: 1,
    totals: {
      servers_live: 4,
      machines_live: 2,
      slots: 13,
      slots_used: 3,
      ...usage({
        reporting: 1,
        players: 6,
        memory_bytes: 200 * MiB,
        memory_limit_bytes: 512 * MiB,
        cpus_used: 0.25,
        cpus_limit: 1,
        egress_bytes_per_second: 150_000,
        egress_bytes_window: 540_000_000,
      }),
    },
    machines: [
      {
        id: 3,
        kind: 'shared',
        state: 'ready',
        instance_type: 't3a.large',
        slots: 12,
        slots_used: 2,
        ...usage({
          reporting: 1,
          players: 6,
          memory_bytes: 200 * MiB,
          memory_limit_bytes: 512 * MiB,
          cpus_used: 0.25,
          cpus_limit: 1,
          egress_bytes_per_second: 150_000,
          egress_bytes_window: 520_000_000,
        }),
      },
      { id: 4, kind: 'own', state: 'ready', instance_type: 'c7a.medium', slots: 1, slots_used: 1, ...usage({ egress_bytes_window: 20_000_000 }) },
    ],
    servers: [
      {
        id: 12,
        name: 'Booting One',
        state: 'booting',
        own_machine: false,
        owner: { id: 2, username: 'owner2' },
        machine_ref: 3,
        slot: 0,
        uptime_seconds: null,
        ...none,
        sample_at: '2026-10-16T09:41:00Z',
        egress_bytes_per_second: 0,
        window: window({ minutes: 1 }),
      },
      {
        id: 7,
        name: 'Night Ops',
        state: 'running',
        own_machine: false,
        owner: { id: 1, username: 'owner1' },
        machine_ref: 3,
        slot: 1,
        uptime_seconds: 3 * 3600 + 5 * 60,
        sample_at: '2026-10-16T09:41:00Z',
        players: 6,
        max_players: 32,
        cpu_percent: 25,
        memory_bytes: 200 * MiB,
        egress_bytes_per_second: 150_000,
        window: window({ minutes: 60, cpu_percent_mean: 18.4, cpu_percent_peak: 61, memory_bytes_peak: 310 * MiB, egress_bytes: 520_000_000 }),
      },
      {
        id: 9,
        name: 'Own Box',
        state: 'running',
        own_machine: true,
        owner: null,
        machine_ref: 4,
        slot: 0,
        uptime_seconds: 600,
        ...none,
        window: window({ minutes: 5, memory_bytes_peak: 100 * MiB, egress_bytes: 20_000_000 }),
      },
      {
        id: 15,
        name: 'Waiting',
        state: 'requested',
        own_machine: false,
        owner: { id: 1, username: 'owner1' },
        machine_ref: null,
        slot: null,
        uptime_seconds: null,
        ...none,
        window: window(),
      },
    ],
  };
}
