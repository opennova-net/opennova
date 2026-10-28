// The admin's background jobs in words (pages/admin/system/JobsTab.vue and its
// views, JobPage.vue): what each kind does, a job's state, an interval, a
// lease, what comes next on a schedule, what a group's retry did, and the
// things a job's payload names.
// Durations arrive as whole seconds and leave as words (utils/format.ts),
// never as raw numbers with a unit code.

import { badgeClass } from './tone';
import type { Job, JobGroupRetry, JobSchedule, JobWorker } from '../types/jobs';
import { formatDuration, formatRelative, timeMs } from './format';

// What last_error says of a job an admin cancelled
// (apps/novaworld_web/jobs/job_admin.h kCancelledByAdmin).
const CANCELLED_BY_ADMIN = 'cancelled by an admin';

const KIND_LABELS: Record<string, string> = {
  'machine.launch': 'Launch a fleet machine',
  'machine.describe': 'Ask the cloud about a fleet machine',
  'machine.terminate': 'End a fleet machine',
  'fleet.sweep': "Match the cloud's machines with the fleet",
  'billing.reconcile': 'Reconcile every subscription with Stripe',
  'billing.sync': 'Sync the customers Stripe sent events for',
  'mail.send': 'Send one mail',
  'hosts.prune': 'Prune stale listings and lobby instances',
  'host_keys.prune': 'Prune idle hosting keys',
  'web_sessions.prune': 'Prune expired website sessions',
  'active_user_sessions.prune': 'Prune idle game logins',
  'login_addresses.prune': 'Prune old login addresses',
  'password_resets.prune': 'Prune expired password reset links',
  'billing_events.prune': 'Prune old billing events',
  'stats.prune': "Prune old hosted servers' stats and ended machines",
  'jobs.prune': 'Prune finished jobs',
  'unknown_messages.prune': 'Prune old unknown messages',
};

// "Launch a fleet machine": what a kind does; the kind itself for one this
// site does not know.
export function jobKindLabel(kind: string): string {
  return KIND_LABELS[kind] ?? kind;
}

export function isCancelled(job: Pick<Job, 'state' | 'last_error'>): boolean {
  return job.state === 'failed' && job.last_error.startsWith(CANCELLED_BY_ADMIN);
}

export function jobStateLabel(job: Pick<Job, 'state' | 'last_error'>): string {
  if (isCancelled(job)) return 'Cancelled';
  switch (job.state) {
    case 'queued':
      return 'Waiting';
    case 'running':
      return 'Running';
    case 'done':
      return 'Done';
    case 'failed':
      return 'Failed';
  }
}

export function jobStateBadgeClass(job: Pick<Job, 'state' | 'last_error'>): string {
  if (isCancelled(job)) return badgeClass('muted');
  return badgeClass(job.state === 'running' ? 'ok' : job.state === 'queued' ? 'warn' : job.state === 'failed' ? 'bad' : 'muted');
}

// "4 seconds", "2 minutes", "1 hour 5 minutes": a length of time given in
// seconds.
export function secondsLabel(seconds: number): string {
  return formatDuration(seconds * 1000);
}

function plural(n: number, unit: string): string {
  return n === 1 ? unit : `${n} ${unit}s`;
}

// "every 30 seconds", "every minute", "every 5 minutes", "every hour", "every
// day": a schedule's interval.
export function everyLabel(seconds: number): string {
  if (seconds > 0 && seconds % 86_400 === 0) return `every ${plural(seconds / 86_400, 'day')}`;
  if (seconds > 0 && seconds % 3600 === 0) return `every ${plural(seconds / 3600, 'hour')}`;
  if (seconds > 0 && seconds % 60 === 0) return `every ${plural(seconds / 60, 'minute')}`;
  return `every ${plural(seconds, 'second')}`;
}

// "2 of 5": a job's attempts so far out of what it may use.
export function attemptsLabel(job: Pick<Job, 'attempts' | 'max_attempts'>): string {
  return `${job.attempts} of ${job.max_attempts}`;
}

// A running job's lease: "runs out in 52 seconds", or "ran out 30 seconds
// ago" once its worker went away; '' for a job holding none.
export function leaseLabel(job: Pick<Job, 'lease_left_seconds'>): string {
  const left = job.lease_left_seconds;
  if (left === null) return '';
  if (left >= 0) return `runs out in ${secondsLabel(left)}`;
  return `ran out ${secondsLabel(-left)} ago`;
}

export function leaseExpired(job: Pick<Job, 'lease_left_seconds'>): boolean {
  return job.lease_left_seconds !== null && job.lease_left_seconds < 0;
}

// What a schedule does next, against `now` (ms): after the one of its kind
// that waits or runs; due now (or, conditional, once there is work for it);
// else when, relative.
export function scheduleNextLabel(schedule: JobSchedule, now: number): string {
  if (schedule.pending) return 'After the one waiting or running';
  const next = timeMs(schedule.next_at);
  if (Number.isNaN(next) || next <= now) return schedule.conditional ? 'When there is work for it' : 'Due now';
  return formatRelative(next, now);
}

// A datetime-local input's value read as UTC: Unix seconds; null for none.
export function utcInputSeconds(value: string): number | null {
  if (!value) return null;
  const ms = Date.parse(`${value.length === 16 ? `${value}:00` : value}Z`);
  return Number.isNaN(ms) ? null : Math.floor(ms / 1000);
}

// Unix seconds as a datetime-local input's value in UTC ("2026-10-16T09:30").
export function utcInputValue(seconds: number): string {
  return new Date(seconds * 1000).toISOString().slice(0, 16);
}

function counted(n: number, one: string, many: string): string {
  return `${n} ${n === 1 ? one : many}`;
}

// A worker process's leases: the ones that ran out (the process went away)
// first; else when the soonest of the others runs out; '' while it holds none.
export function workerLeaseLabel(worker: Pick<JobWorker, 'lapsed' | 'lease_left_seconds'>): string {
  if (worker.lapsed > 0) return `${counted(worker.lapsed, 'lease', 'leases')} ran out`;
  if (worker.lease_left_seconds === null) return '';
  return `Soonest runs out in ${secondsLabel(worker.lease_left_seconds)}`;
}

// What a failure group's retry did, in a sentence: "2 jobs queued again; 1
// cannot run again."
export function groupRetryMessage(result: JobGroupRetry): string {
  const skipped = [
    ...(result.secret_dropped > 0 ? [`${result.secret_dropped} cannot run again`] : []),
    ...(result.already_pending > 0 ? [`${result.already_pending} ${result.already_pending === 1 ? 'waits' : 'wait'} behind another`] : []),
    ...(result.changed > 0 ? [`${result.changed} already left the failed state`] : []),
  ];
  const retried = `${counted(result.retried, 'job', 'jobs')} queued again`;
  return skipped.length ? `${retried}; ${skipped.join(', ')}.` : `${retried}.`;
}

export type PayloadLinkKind = 'machine' | 'server' | 'player' | 'product' | 'job';

// A thing a job's payload names by its id, for a link to its page.
export interface PayloadLink {
  kind: PayloadLinkKind;
  id: number;
  name: string;
}

// The id keys a payload may carry, and what each names.
const PAYLOAD_IDS: Record<string, PayloadLinkKind> = {
  machine_id: 'machine',
  server_id: 'server',
  hosted_server_id: 'server',
  user_id: 'player',
  account_id: 'player',
  owner_id: 'player',
  product_id: 'product',
  job_id: 'job',
};

function isRecord(v: unknown): v is Record<string, unknown> {
  return typeof v === 'object' && v !== null && !Array.isArray(v);
}

function idOf(v: unknown): number | null {
  if (typeof v === 'number' && Number.isInteger(v) && v > 0) return v;
  if (typeof v === 'string' && /^[1-9]\d{0,15}$/.test(v)) return Number(v);
  return null;
}

function text(v: unknown): string {
  return typeof v === 'string' ? v.trim() : '';
}

// A linked thing's name, from what the same object says beside its id.
function payloadName(kind: PayloadLinkKind, id: number, at: Record<string, unknown>): string {
  switch (kind) {
    case 'server':
      return text(at.game_name) || text(at.server_name) || `Server ${id}`;
    case 'player':
      return text(at.username) || `Account ${id}`;
    case 'product':
      return text(at.product_name) || text(at.product_key) || `Product ${id}`;
    case 'machine':
      return `Machine ${id}`;
    case 'job':
      return `Job ${id}`;
  }
}

// The machines, servers, players, products and jobs a job's payload names by
// id, at its top level and one level down, each once, in the payload's order.
export function payloadLinks(payload: unknown): PayloadLink[] {
  if (!isRecord(payload)) return [];
  const out: PayloadLink[] = [];
  const seen = new Set<string>();
  const scan = (at: Record<string, unknown>) => {
    for (const [key, value] of Object.entries(at)) {
      const kind = PAYLOAD_IDS[key];
      const id = kind ? idOf(value) : null;
      if (!kind || id === null || seen.has(`${kind} ${id}`)) continue;
      seen.add(`${kind} ${id}`);
      out.push({ kind, id, name: payloadName(kind, id, at) });
    }
  };
  scan(payload);
  for (const value of Object.values(payload)) if (isRecord(value)) scan(value);
  return out;
}
