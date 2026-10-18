// The log viewer's arithmetic and words (components/LogViewer.vue): a follow's
// newer lines after the ones shown, where a page was read, a line's time, and
// the filter the route takes.

import type { LogLine, LogPage } from '../types/logs';

// The route's filter: at most 200 bytes, no quote or backslash (a CloudWatch
// filter pattern's term is quoted text).
export const LOG_FILTER_MAX_BYTES = 200;

// Why a filter cannot be sent, or '' when it can.
export function logFilterProblem(q: string): string {
  if (new TextEncoder().encode(q).length > LOG_FILTER_MAX_BYTES) return 'The filter is at most 200 bytes.';
  if (/["\\]/.test(q)) return 'The filter cannot hold a quote or a backslash.';
  return '';
}

// The lines shown with a follow's newest page after them: the page's lines past
// the last one shown. Null when the page does not reach back to it (more lines
// came than a page holds, or the log started over): the caller shows the page
// alone, and older lines load from its cursor.
export function appendNewer(shown: readonly LogLine[], fresh: readonly LogLine[]): LogLine[] | null {
  if (shown.length === 0) return [...fresh];
  const last = shown[shown.length - 1];
  for (let i = fresh.length - 1; i >= 0; i--) {
    if (fresh[i].at === last.at && fresh[i].text === last.text) return [...shown, ...fresh.slice(i + 1)];
  }
  return null;
}

// "CloudWatch /opennova/prod/servers, stream 12/g2", "The log directory
// /var/log/opennova, servers/12/g2.log": where the page was read.
export function logSourceText(page: Pick<LogPage, 'source' | 'group' | 'stream'>): string {
  if (page.group === null) return 'Nothing to read yet: no log names this yet.';
  if (page.source === 'cloudwatch') return `CloudWatch ${page.group}${page.stream ? `, stream ${page.stream}` : ', every stream of it'}`;
  return `The log directory ${page.group}${page.stream ? `, ${page.stream}` : ''}`;
}

// "2026-10-18 14:05:03": a line's time to the second (UTC, as the line was
// stamped); '' for a line with none.
export function lineTime(at: string | null): string {
  if (!at || at.length < 19) return at ?? '';
  return `${at.slice(0, 10)} ${at.slice(11, 19)}`;
}
