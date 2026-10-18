// What everything uses (apps/novaworld_web/infra/usage_routes.h): GET
// /api/admin/usage for a hosted server, a machine, a program (or one of its
// tasks) and an AWS resource, and GET /api/admin/programs, every program the
// service runs with what it uses now. Each answers an AdminResult (admin.ts); a
// reply that does not read as the route's is malformed.

import { adminClient } from './client';
import { adminCall, type AdminResult } from './admin';
import type {
  MetricPoint,
  ProgramTask,
  ProgramUsage,
  ProgramsResponse,
  ResourceId,
  ResourceUsage,
  Usage,
  UsageNow,
  UsagePoint,
  UsageRange,
  UsageSubject,
} from '../types/usage';

type Fields = Record<string, unknown>;

const isObject = (v: unknown): v is Fields => typeof v === 'object' && v !== null && !Array.isArray(v);
const numberOrNull = (v: unknown): number | null => (typeof v === 'number' && Number.isFinite(v) ? v : null);
const textOrNull = (v: unknown): string | null => (typeof v === 'string' && v !== '' ? v : null);
const isCount = (v: unknown): v is number => Number.isInteger(v) && (v as number) >= 0;

const SUBJECTS: ReadonlySet<unknown> = new Set<UsageSubject>(['server', 'machine', 'program']);
const RANGES: ReadonlySet<unknown> = new Set<UsageRange>(['48h', '400d']);
const RESOURCES: ReadonlySet<unknown> = new Set<ResourceId>(['rds', 'cache', 'nlb']);

function pickPoint(v: unknown, subject: UsageSubject): UsagePoint | null {
  if (!isObject(v) || typeof v.at !== 'string') return null;
  const point: UsagePoint = {
    at: v.at,
    cpu_percent: numberOrNull(v.cpu_percent),
    memory_bytes: numberOrNull(v.memory_bytes),
    net_in_bps: numberOrNull(v.net_in_bps),
    net_out_bps: numberOrNull(v.net_out_bps),
  };
  if (subject === 'server') {
    point.tick_p99_ms = numberOrNull(v.tick_p99_ms);
    point.late_ticks = numberOrNull(v.late_ticks);
  }
  if (subject === 'machine') point.disk_free_bytes = numberOrNull(v.disk_free_bytes);
  return point;
}

export function pickUsage(v: unknown): Usage | null {
  if (!isObject(v) || !SUBJECTS.has(v.subject) || !RANGES.has(v.range) || !Array.isArray(v.series)) return null;
  if (typeof v.id !== 'string' && typeof v.id !== 'number') return null;
  const subject = v.subject as UsageSubject;
  const series: UsagePoint[] = [];
  for (const p of v.series) {
    const point = pickPoint(p, subject);
    if (point === null) return null;
    series.push(point);
  }
  const limit: Fields = isObject(v.limit) ? v.limit : {};
  return {
    subject,
    id: String(v.id),
    task: textOrNull(v.task),
    range: v.range as UsageRange,
    step_seconds: isCount(v.step_seconds) ? v.step_seconds : v.range === '400d' ? 3600 : 60,
    limit: { cpu: numberOrNull(limit.cpu), memory_bytes: numberOrNull(limit.memory_bytes) },
    series,
    now: v.now == null ? null : pickPoint(v.now, subject),
    disk_total_bytes: numberOrNull(v.disk_total_bytes),
  };
}

// GET /api/admin/usage?subject=&id=&range=[&task=]: a server's, a machine's or
// a program's (its one task's with `task`) usage over the range.
export function fetchUsage(
  subject: UsageSubject,
  id: string | number,
  range: UsageRange,
  options: { task?: string; signal?: AbortSignal } = {},
): Promise<AdminResult<Usage>> {
  const params: Record<string, string> = { subject, id: String(id), range };
  if (options.task) params.task = options.task;
  return adminCall(() => adminClient.get<unknown>('/usage', { params, signal: options.signal }), pickUsage);
}

function pickMetricPoints(v: unknown): MetricPoint[] | null {
  if (!Array.isArray(v)) return null;
  const out: MetricPoint[] = [];
  for (const p of v) {
    if (!isObject(p) || typeof p.at !== 'string' || numberOrNull(p.value) === null) return null;
    out.push({ at: p.at, value: p.value as number });
  }
  return out;
}

export function pickResourceUsage(v: unknown): ResourceUsage | null {
  if (!isObject(v) || !RESOURCES.has(v.id) || !RANGES.has(v.range) || typeof v.available !== 'boolean') return null;
  const statistics: Record<string, string> = {};
  if (isObject(v.statistics)) {
    for (const [name, s] of Object.entries(v.statistics)) if (typeof s === 'string') statistics[name] = s;
  }
  const metrics: Record<string, MetricPoint[]> = {};
  if (v.metrics !== undefined && v.metrics !== null) {
    if (!isObject(v.metrics)) return null;
    for (const [name, points] of Object.entries(v.metrics)) {
      const picked = pickMetricPoints(points);
      if (picked === null) return null;
      metrics[name] = picked;
    }
  }
  return {
    id: v.id as ResourceId,
    range: v.range as UsageRange,
    available: v.available,
    unavailable: textOrNull(v.unavailable),
    error: textOrNull(v.error),
    period_seconds: isCount(v.period_seconds) ? v.period_seconds : v.range === '400d' ? 3600 : 60,
    statistics,
    metrics,
  };
}

// GET /api/admin/usage?subject=infra&id=rds|cache|nlb&range=: an AWS resource's
// CloudWatch metrics (none on the local stack, which the reply says).
export function fetchResourceUsage(id: ResourceId, range: UsageRange, signal?: AbortSignal): Promise<AdminResult<ResourceUsage>> {
  return adminCall(
    () => adminClient.get<unknown>('/usage', { params: { subject: 'infra', id, range }, signal }),
    pickResourceUsage,
  );
}

function pickNow(v: Fields): UsageNow {
  return {
    cpu_percent: numberOrNull(v.cpu_percent),
    cpu_limit: numberOrNull(v.cpu_limit),
    memory_bytes: numberOrNull(v.memory_bytes),
    memory_limit_bytes: numberOrNull(v.memory_limit_bytes),
    net_in_bps: numberOrNull(v.net_in_bps),
    net_out_bps: numberOrNull(v.net_out_bps),
  };
}

function pickTask(v: unknown): ProgramTask | null {
  if (!isObject(v) || typeof v.id !== 'string' || v.id === '') return null;
  return {
    id: v.id,
    reporting: v.reporting === true,
    sampled_at: textOrNull(v.sampled_at),
    ...pickNow(v),
    last_status: textOrNull(v.last_status),
    health: textOrNull(v.health),
    started_at: textOrNull(v.started_at),
    task_definition: textOrNull(v.task_definition),
  };
}

function pickProgram(v: unknown): ProgramUsage | null {
  if (!isObject(v) || typeof v.name !== 'string' || !isCount(v.running) || !Array.isArray(v.tasks)) return null;
  const tasks: ProgramTask[] = [];
  for (const t of v.tasks) {
    const task = pickTask(t);
    if (task === null) return null;
    tasks.push(task);
  }
  return {
    name: v.name,
    running: v.running,
    desired: isCount(v.desired) ? v.desired : null,
    release: textOrNull(v.release),
    on_release: typeof v.on_release === 'boolean' ? v.on_release : null,
    ...pickNow(v),
    restarts_24h: isCount(v.restarts_24h) ? v.restarts_24h : 0,
    tasks,
  };
}

export function pickPrograms(v: unknown): ProgramsResponse | null {
  if (!isObject(v) || typeof v.at !== 'string' || !Array.isArray(v.programs)) return null;
  const programs: ProgramUsage[] = [];
  for (const p of v.programs) {
    const program = pickProgram(p);
    if (program === null) return null;
    programs.push(program);
  }
  const cloud: Fields = isObject(v.cloud) ? v.cloud : {};
  return {
    at: v.at,
    cloud: { configured: cloud.configured === true, unavailable: textOrNull(cloud.unavailable), error: textOrNull(cloud.error) },
    programs,
  };
}

// GET /api/admin/programs.
export function fetchPrograms(signal?: AbortSignal): Promise<AdminResult<ProgramsResponse>> {
  return adminCall(() => adminClient.get<unknown>('/programs', { signal }), pickPrograms);
}
