// What everything uses, as the admin's usage routes answer it
// (apps/novaworld_web/infra/usage_routes.h): GET /api/admin/usage for a hosted
// server, a machine, a program or an AWS resource, and GET /api/admin/programs.
// Times are ISO 8601 UTC; rates are bytes a second; a value the samples did not
// hold is null.

export type UsageSubject = 'server' | 'machine' | 'program';

// 1-minute points over 48 hours, or hourly points over 400 days.
export type UsageRange = '48h' | '400d';

export interface UsagePoint {
  at: string;
  // In percent of the limit's CPUs.
  cpu_percent: number | null;
  memory_bytes: number | null;
  net_in_bps: number | null;
  net_out_bps: number | null;
  // A hosted server's tick health: its frames' 99th percentile time and how
  // many started late (the minute's, or the hour's worst and sum).
  tick_p99_ms?: number | null;
  late_ticks?: number | null;
  // A machine's free disk (the hour's least).
  disk_free_bytes?: number | null;
}

// GET /api/admin/usage?subject=server|machine|program.
export interface Usage {
  subject: UsageSubject;
  // The server's or machine's row id, or the program's name.
  id: string;
  // A program's one task; null for all of them together.
  task: string | null;
  range: UsageRange;
  step_seconds: number;
  // What the subject may use: its CPUs (null where none is known: a
  // machine's) and its memory.
  limit: { cpu: number | null; memory_bytes: number | null };
  // Oldest first.
  series: UsagePoint[];
  // The newest minute while it is at most 3 minutes old, else null.
  now: UsagePoint | null;
  // A machine's disk; null for the others.
  disk_total_bytes: number | null;
}

// The deployment's AWS resources whose CloudWatch metrics the view reads.
export type ResourceId = 'rds' | 'cache' | 'nlb';

export interface MetricPoint {
  at: string;
  value: number;
}

// GET /api/admin/usage?subject=infra&id=rds|cache|nlb.
export interface ResourceUsage {
  id: ResourceId;
  range: UsageRange;
  // False on the local stack, which reads no AWS (`unavailable` says why).
  available: boolean;
  unavailable: string | null;
  // AWS's words when the read failed.
  error: string | null;
  period_seconds: number;
  // Each metric's statistic: Average, Sum, Minimum or Maximum.
  statistics: Record<string, string>;
  // AWS's metric names ("HealthyHostCount:<listener>" for a target group's),
  // each oldest first.
  metrics: Record<string, MetricPoint[]>;
}

// What a program's task, or its reporting tasks together, use now.
export interface UsageNow {
  cpu_percent: number | null;
  // CPUs.
  cpu_limit: number | null;
  memory_bytes: number | null;
  memory_limit_bytes: number | null;
  net_in_bps: number | null;
  net_out_bps: number | null;
}

// One of a program's tasks: a reporting one (it sampled itself in the last 3
// minutes) or one ECS runs that does not report.
export interface ProgramTask extends UsageNow {
  id: string;
  reporting: boolean;
  sampled_at: string | null;
  // ECS's view, null where it is not known (the local stack).
  last_status: string | null;
  health: string | null;
  started_at: string | null;
  task_definition: string | null;
}

export interface ProgramUsage extends UsageNow {
  name: string;
  // ECS's counts; with no cloud, the tasks reporting now and null.
  running: number;
  desired: number | null;
  // The revision its service runs (family:revision), null without the cloud.
  release: string | null;
  // Whether that is the release's; null when unknown.
  on_release: boolean | null;
  // Its tasks that reported in the last 24 hours and no longer do (a crash's,
  // a deploy's, a scale-in's).
  restarts_24h: number;
  tasks: ProgramTask[];
}

// GET /api/admin/programs.
export interface ProgramsResponse {
  at: string;
  // Whether ECS was read (the local stack's is not: `unavailable` says why).
  cloud: { configured: boolean; unavailable: string | null; error: string | null };
  // In the service's order.
  programs: ProgramUsage[];
}
