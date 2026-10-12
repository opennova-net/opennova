// The service's UTC times ('YYYY-MM-DD HH:MM:SS', SQLite's CURRENT_TIMESTAMP)
// for display.

// The time as an ISO instant, for a <time datetime>.
export function isoOf(utc: string): string {
  return `${utc.replace(' ', 'T')}Z`;
}

function parse(utc: string): number {
  return Date.parse(isoOf(utc));
}

// The time in the visitor's own zone and format.
export function localOf(utc: string): string {
  const t = parse(utc);
  return Number.isNaN(t) ? utc : new Date(t).toLocaleString();
}

// "12 s", "5 min", "3 h", "2 d": how long ago the time was, against `now` (ms).
export function durationSince(utc: string, now: number): string {
  const t = parse(utc);
  if (Number.isNaN(t)) return utc;
  const seconds = Math.max(0, Math.round((now - t) / 1000));
  if (seconds < 60) return `${seconds} s`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)} min`;
  if (seconds < 86400) return `${Math.floor(seconds / 3600)} h`;
  return `${Math.floor(seconds / 86400)} d`;
}

// "5 min ago".
export function ago(utc: string, now: number): string {
  const t = parse(utc);
  if (Number.isNaN(t)) return utc;
  return `${durationSince(utc, now)} ago`;
}
