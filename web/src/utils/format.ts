// Times, durations and amounts for display, in words a reader needs no key
// for: never raw milliseconds, never an unexplained abbreviation. A time shows
// relative to now ("3 min ago") with its absolute UTC time on hover
// (components/RelativeTime.vue).

// A time the service sent, as epoch milliseconds: its UTC 'YYYY-MM-DD
// HH:MM:SS', an ISO 8601 string, or epoch milliseconds already. NaN when it
// reads as none of them.
export function timeMs(value: string | number): number {
  if (typeof value === 'number') return value;
  const text = value.trim();
  if (/^\d{4}-\d{2}-\d{2} \d{2}:\d{2}(:\d{2})?$/.test(text)) return Date.parse(`${text.replace(' ', 'T')}Z`);
  return Date.parse(text);
}

function plural(n: number, unit: string): string {
  return `${n} ${unit}${n === 1 ? '' : 's'}`;
}

// "45 seconds", "4 minutes", "1 hour 5 minutes", "3 days 2 hours": a length of
// time, in its largest unit and the next one when that is not zero.
export function formatDuration(ms: number): string {
  if (!Number.isFinite(ms) || ms < 0) return 'unknown';
  const seconds = Math.round(ms / 1000);
  if (seconds < 1) return 'less than a second';
  if (seconds < 60) return plural(seconds, 'second');
  const minutes = Math.floor(seconds / 60);
  if (minutes < 60) return plural(minutes, 'minute');
  const hours = Math.floor(minutes / 60);
  if (hours < 24) {
    const rest = minutes % 60;
    return rest ? `${plural(hours, 'hour')} ${plural(rest, 'minute')}` : plural(hours, 'hour');
  }
  const days = Math.floor(hours / 24);
  const rest = hours % 24;
  return rest ? `${plural(days, 'day')} ${plural(rest, 'hour')}` : plural(days, 'day');
}

// "just now", "3 min ago", "2 hours ago", "5 days ago", or ahead of `now`
// "in 3 days": a time against `now` (ms). The text itself when unreadable.
export function formatRelative(value: string | number, now: number): string {
  const t = timeMs(value);
  if (Number.isNaN(t)) return String(value);
  const diff = now - t;
  const ahead = diff < 0;
  const seconds = Math.round(Math.abs(diff) / 1000);
  if (seconds < 45) return ahead ? 'in a moment' : 'just now';
  let span: string;
  if (seconds < 3600) span = `${Math.max(1, Math.round(seconds / 60))} min`;
  else if (seconds < 86_400) span = plural(Math.round(seconds / 3600), 'hour');
  else span = plural(Math.round(seconds / 86_400), 'day');
  return ahead ? `in ${span}` : `${span} ago`;
}

function pad(n: number): string {
  return String(n).padStart(2, '0');
}

// "2026-10-14 13:05 UTC": the absolute time, for a hover.
export function formatUtc(value: string | number): string {
  const t = timeMs(value);
  if (Number.isNaN(t)) return String(value);
  const d = new Date(t);
  return `${d.getUTCFullYear()}-${pad(d.getUTCMonth() + 1)}-${pad(d.getUTCDate())} ${pad(d.getUTCHours())}:${pad(d.getUTCMinutes())} UTC`;
}

// "14 October 2026": a date alone, in the visitor's own format.
export function formatDate(value: string | number): string {
  const t = timeMs(value);
  if (Number.isNaN(t)) return String(value);
  return new Date(t).toLocaleDateString(undefined, { day: 'numeric', month: 'long', year: 'numeric', timeZone: 'UTC' });
}

// The ISO instant, for a <time datetime>; '' when unreadable.
export function isoTime(value: string | number): string {
  const t = timeMs(value);
  return Number.isNaN(t) ? '' : new Date(t).toISOString();
}
