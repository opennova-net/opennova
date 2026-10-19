// What needs an admin (GET /api/admin/attention, fleet_health.h): the fleet's
// findings and the deployment's firing alarms. Answers an AdminResult
// (admin.ts); a finding of a level this site does not know reads as a warning,
// so nothing the service flags is dropped.

import { adminClient } from './client';
import { adminCall, type AdminResult } from './admin';
import type { Attention, FindingLevel, FiringAlarm, Finding } from '../types/attention';

type Fields = Record<string, unknown>;

const isObject = (v: unknown): v is Fields => typeof v === 'object' && v !== null && !Array.isArray(v);
const textOrNull = (v: unknown): string | null => (typeof v === 'string' && v !== '' ? v : null);

const LEVELS: ReadonlySet<unknown> = new Set<FindingLevel>(['danger', 'warn', 'info']);

function pickFinding(v: unknown): Finding | null {
  if (!isObject(v) || typeof v.text !== 'string') return null;
  return {
    level: LEVELS.has(v.level) ? (v.level as FindingLevel) : 'warn',
    subject: typeof v.subject === 'string' ? v.subject : '',
    text: v.text,
    since: textOrNull(v.since),
    link: textOrNull(v.link),
    source: typeof v.source === 'string' ? v.source : '',
  };
}

function pickAlarm(v: unknown): FiringAlarm | null {
  if (!isObject(v) || typeof v.name !== 'string') return null;
  return {
    name: v.name,
    state_since: textOrNull(v.state_since),
    description: typeof v.description === 'string' ? v.description : '',
  };
}

export function pickAttention(v: unknown): Attention | null {
  if (!isObject(v) || !Array.isArray(v.findings) || typeof v.alarms_read !== 'boolean' || typeof v.alarms_read_failed !== 'boolean') {
    return null;
  }
  const findings: Finding[] = [];
  for (const f of v.findings) {
    const finding = pickFinding(f);
    if (finding === null) return null;
    findings.push(finding);
  }
  const alarms: FiringAlarm[] = [];
  for (const a of Array.isArray(v.alarms) ? v.alarms : []) {
    const alarm = pickAlarm(a);
    if (alarm === null) return null;
    alarms.push(alarm);
  }
  return {
    findings,
    alarms,
    alarms_read: v.alarms_read,
    alarms_read_failed: v.alarms_read_failed,
    checked_at: typeof v.checked_at === 'string' ? v.checked_at : '',
  };
}

// GET /api/admin/attention.
export function fetchAttention(signal?: AbortSignal): Promise<AdminResult<Attention>> {
  return adminCall(() => adminClient.get<unknown>('/attention', { signal }), pickAttention);
}

// What the admin nav's count counts: the danger and warn findings and the
// firing alarms (the info findings need no one).
export function attentionCount(a: Attention): number {
  return a.findings.filter((f) => f.level !== 'info').length + a.alarms.length;
}
