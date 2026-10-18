// Every log on the site, as the admin's log route answers it
// (apps/novaworld_web/infra/log_routes.h, GET /api/admin/logs): a page of one
// log from the store the service names, a hosted server's start, a machine's
// hostd or a program's tasks.

export type LogSubject = 'server' | 'machine' | 'program';

export interface LogQuery {
  subject: LogSubject;
  // The server's or machine's row id, or the program's name.
  id: string | number;
  // A server's start (its newest without one).
  generation?: number;
  // A program's one task (every task without one).
  task?: string;
  // An earlier page's cursor: the lines older than it.
  before?: string;
  // 1 to 500 (200).
  limit?: number;
  // The lines holding this text.
  q?: string;
}

export interface LogLine {
  // ISO 8601 UTC with milliseconds; null for a line with no time.
  at: string | null;
  text: string;
}

export interface LogPage {
  subject: LogSubject;
  id: string;
  // The server's start read; null for none yet.
  generation: number | null;
  // The program's one task; null for every task.
  task: string | null;
  // Where it was read: CloudWatch, or the local stack's directory.
  source: 'cloudwatch' | 'file';
  // The CloudWatch group and stream (or a program's streams' prefix), or the
  // directory and the file under it; null with nothing to read yet.
  group: string | null;
  stream: string | null;
  // Oldest first.
  lines: LogLine[];
  // The cursor of the lines older than these; null for none.
  before: string | null;
}
