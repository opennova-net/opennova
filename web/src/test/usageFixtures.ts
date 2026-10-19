// The admin's usage, programs, logs and attention replies for the tests, shaped
// as apps/novaworld_web/infra/usage_routes.cpp, log_routes.cpp and the
// attention route answer them. NOW is the replies' present: every series ends
// at it.

import type { Attention, Finding } from '../types/attention';
import type { LogLine, LogPage } from '../types/logs';
import type { ProgramTask, ProgramUsage, ProgramsResponse, ResourceUsage, Usage, UsagePoint } from '../types/usage';

export const NOW = Date.UTC(2026, 9, 18, 12, 0, 0);

const MIB = 1024 * 1024;

// An ISO time `seconds` before NOW, as the routes write it.
export function ago(seconds: number): string {
  return new Date(NOW - seconds * 1000).toISOString().replace(/\.\d{3}Z$/, 'Z');
}

function point(secondsAgo: number, overrides: Partial<UsagePoint> = {}): UsagePoint {
  return {
    at: ago(secondsAgo),
    cpu_percent: 41,
    memory_bytes: 612 * MIB,
    net_in_bps: 300_000,
    net_out_bps: 1_200_000,
    ...overrides,
  };
}

// A hosted server's last three minutes, its slot's limits (1 vCPU, 512 MiB).
export function serverUsage(overrides: Partial<Usage> = {}): Usage {
  const tick = { memory_bytes: 312 * MIB, tick_p99_ms: 14.2, late_ticks: 0 };
  const series = [point(120, { ...tick, net_in_bps: null, net_out_bps: null }), point(60, tick), point(0, { ...tick, late_ticks: 3 })];
  return {
    subject: 'server',
    id: '12',
    task: null,
    range: '48h',
    step_seconds: 60,
    limit: { cpu: 1, memory_bytes: 512 * MIB },
    series,
    now: series[2],
    disk_total_bytes: null,
    ...overrides,
  };
}

// A machine's last three minutes: no CPU limit, its memory and its disk.
export function machineUsage(overrides: Partial<Usage> = {}): Usage {
  const series = [120, 60, 0].map((s) => point(s, { cpu_percent: 22, memory_bytes: 2048 * MIB, disk_free_bytes: 30_000_000_000 }));
  return {
    subject: 'machine',
    id: '3',
    task: null,
    range: '48h',
    step_seconds: 60,
    limit: { cpu: null, memory_bytes: 4096 * MIB },
    series,
    now: series[2],
    disk_total_bytes: 40_000_000_000,
    ...overrides,
  };
}

// The web program's two tasks together (0.5 vCPU and 1 GiB each).
export function programUsage(overrides: Partial<Usage> = {}): Usage {
  const series = [120, 60, 0].map((s) => point(s));
  return {
    subject: 'program',
    id: 'web',
    task: null,
    range: '48h',
    step_seconds: 60,
    limit: { cpu: 1, memory_bytes: 2048 * MIB },
    series,
    now: series[2],
    disk_total_bytes: null,
    ...overrides,
  };
}

export function resourceUsage(overrides: Partial<ResourceUsage> = {}): ResourceUsage {
  return {
    id: 'rds',
    range: '48h',
    available: true,
    unavailable: null,
    error: null,
    period_seconds: 60,
    statistics: { CPUUtilization: 'Average', FreeableMemory: 'Average', DatabaseConnections: 'Average' },
    metrics: {
      CPUUtilization: [
        { at: ago(120), value: 4.5 },
        { at: ago(60), value: 5.25 },
      ],
      FreeableMemory: [
        { at: ago(120), value: 400 * MIB },
        { at: ago(60), value: 380 * MIB },
      ],
      DatabaseConnections: [
        { at: ago(120), value: 12 },
        { at: ago(60), value: 14 },
      ],
    },
    ...overrides,
  };
}

// The local stack's: AWS is not read.
export function localResourceUsage(id: ResourceUsage['id'] = 'rds'): ResourceUsage {
  return {
    id,
    range: '48h',
    available: false,
    unavailable: 'AWS is not read here: no ONNET_INFRA_REGION',
    error: null,
    period_seconds: 60,
    statistics: {},
    metrics: {},
  };
}

export function programTask(id: string, overrides: Partial<ProgramTask> = {}): ProgramTask {
  return {
    id,
    reporting: true,
    sampled_at: ago(30),
    cpu_percent: 41,
    cpu_limit: 0.5,
    memory_bytes: 612 * MIB,
    memory_limit_bytes: 1024 * MIB,
    net_in_bps: 300_000,
    net_out_bps: 1_200_000,
    last_status: 'RUNNING',
    health: 'HEALTHY',
    started_at: ago(5000),
    task_definition: 'opennova-prod-web:43',
    ...overrides,
  };
}

export function program(name: string, overrides: Partial<ProgramUsage> = {}): ProgramUsage {
  const task = programTask(`${name}0000aaaa1111`, { task_definition: `opennova-prod-${name}:43` });
  return {
    name,
    running: 1,
    desired: 1,
    release: `opennova-prod-${name}:43`,
    on_release: true,
    cpu_percent: 41,
    cpu_limit: 0.5,
    memory_bytes: 612 * MIB,
    memory_limit_bytes: 1024 * MIB,
    net_in_bps: 300_000,
    net_out_bps: 1_200_000,
    restarts_24h: 0,
    tasks: [task],
    ...overrides,
  };
}

// The six programs on a deployment, worker with a restart and data short of
// its task.
export function programsResponse(overrides: Partial<ProgramsResponse> = {}): ProgramsResponse {
  return {
    at: ago(0),
    cloud: { configured: true, unavailable: null, error: null },
    programs: [
      program('gate'),
      program('lobby'),
      program('web'),
      program('fleet'),
      program('worker', { restarts_24h: 1 }),
      program('data', {
        running: 0,
        cpu_percent: null,
        cpu_limit: null,
        memory_bytes: null,
        memory_limit_bytes: null,
        net_in_bps: null,
        net_out_bps: null,
        tasks: [],
      }),
    ],
    ...overrides,
  };
}

export function logLine(secondsAgo: number, text: string): LogLine {
  return { at: new Date(NOW - secondsAgo * 1000).toISOString(), text };
}

export function logPage(overrides: Partial<LogPage> = {}): LogPage {
  return {
    subject: 'server',
    id: '12',
    generation: 2,
    task: null,
    source: 'cloudwatch',
    group: '/opennova/prod/servers',
    stream: '12/g2',
    lines: [logLine(60, '[serve] serving on UDP 32768'), logLine(30, '[serve] player joined')],
    before: 'f/123',
    ...overrides,
  };
}

export function finding(level: Finding['level'], text: string, overrides: Partial<Finding> = {}): Finding {
  return { level, subject: 'machine 3', text, since: ago(600), link: '/admin/fleet/machines/3', source: 'fleet_machines', ...overrides };
}

export function attention(overrides: Partial<Attention> = {}): Attention {
  return {
    findings: [
      finding('danger', 'Machine 3 has not reported for 10 minutes; the brake holds it.'),
      finding('warn', 'Machine 4 runs an older hostd than the release.', { subject: 'machine 4', link: '/admin/fleet/machines/4' }),
      finding('info', 'Launches of shared machines are held until 12:05.', { subject: 'shared machines', link: null }),
    ],
    alarms: [{ name: 'opennova-prod-machines-lost', state_since: ago(300), description: 'A machine was lost. See the Fleet page.' }],
    alarms_read: true,
    checked_at: ago(0),
    ...overrides,
  };
}
