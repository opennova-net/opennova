// Every log on the site (apps/novaworld_web/infra/log_routes.h): GET
// /api/admin/logs, a page of a hosted server's start, a machine's hostd or a
// program's tasks, newest lines last, with the cursor of the older ones. Answers
// an AdminResult (admin.ts). The two refusals that are the store's own
// (no_log_store: why there is none; logs_unavailable: AWS's words, which name an
// action and a resource, never a credential) carry the service's reason after
// ours, the one place an admin needs it.

import { isAxiosError } from 'axios';
import { adminClient } from './client';
import { describeAdminError, malformedAdminReply, type AdminResult } from './admin';
import type { LogLine, LogPage, LogQuery, LogSubject } from '../types/logs';

type Fields = Record<string, unknown>;

const isObject = (v: unknown): v is Fields => typeof v === 'object' && v !== null && !Array.isArray(v);
const textOrNull = (v: unknown): string | null => (typeof v === 'string' && v !== '' ? v : null);

const SUBJECTS: ReadonlySet<unknown> = new Set<LogSubject>(['server', 'machine', 'program']);

export function pickLogPage(v: unknown): LogPage | null {
  if (!isObject(v) || !SUBJECTS.has(v.subject) || !Array.isArray(v.lines)) return null;
  if (typeof v.id !== 'string' && typeof v.id !== 'number') return null;
  const lines: LogLine[] = [];
  for (const l of v.lines) {
    if (!isObject(l) || typeof l.text !== 'string') return null;
    lines.push({ at: textOrNull(l.at), text: l.text });
  }
  return {
    subject: v.subject as LogSubject,
    id: String(v.id),
    generation: Number.isInteger(v.generation) ? (v.generation as number) : null,
    task: textOrNull(v.task),
    source: v.source === 'cloudwatch' ? 'cloudwatch' : 'file',
    group: textOrNull(v.group),
    stream: textOrNull(v.stream),
    lines,
    before: textOrNull(v.before),
  };
}

// GET /api/admin/logs?subject=&id=[&generation=][&task=][&before=][&limit=][&q=].
export async function fetchLogs(query: LogQuery, signal?: AbortSignal): Promise<AdminResult<LogPage>> {
  const params: Record<string, string> = { subject: query.subject, id: String(query.id) };
  if (query.generation !== undefined) params.generation = String(query.generation);
  if (query.task) params.task = query.task;
  if (query.before) params.before = query.before;
  if (query.limit !== undefined) params.limit = String(query.limit);
  if (query.q) params.q = query.q;
  try {
    const res = await adminClient.get<unknown>('/logs', { params, signal });
    const page = pickLogPage(res.data);
    return page === null ? malformedAdminReply(res.status) : { ok: true, value: page };
  } catch (err) {
    const error = describeAdminError(err);
    if ((error.code === 'no_log_store' || error.code === 'logs_unavailable') && isAxiosError(err)) {
      const reason = (err.response?.data as { message?: unknown } | undefined)?.message;
      if (typeof reason === 'string' && reason !== '') error.message = `${error.message} The service says: ${reason}`;
    }
    return { ok: false, error };
  }
}
