// The Overview's words for each area (pages/admin/OverviewSection.vue): which of
// what needs an admin falls in each area (by the page its finding links to),
// and the area's state at a glance, the worst of what it holds, in a tone and
// words.

import type { Attention } from '../types/attention';
import { ADMIN_AREAS, isUnder, type AdminArea } from './adminNav';
import type { Tone } from './infraView';

export interface AreaStatus {
  tone: Tone;
  label: string;
}

const RANK: Record<Tone, number> = { idle: 0, ok: 1, warn: 2, bad: 3 };

// The worst of `states` (the first of the worst, so callers list the most
// telling first); `fallback` when there is none.
export function worstStatus(states: readonly (AreaStatus | null | undefined)[], fallback: AreaStatus): AreaStatus {
  let worst: AreaStatus | null = null;
  for (const s of states) if (s && (worst === null || RANK[s.tone] > RANK[worst.tone])) worst = s;
  return worst ?? fallback;
}

// The area a finding's page belongs to ("/admin/fleet?tab=roll": Hosting);
// null for a link to no admin page.
export function areaOfLink(link: string | null): AdminArea['id'] | null {
  if (!link) return null;
  const path = link.split(/[?#]/)[0];
  return ADMIN_AREAS.find((a) => a.sections.some((s) => isUnder(path, s)))?.id ?? null;
}

// "1 needs you", "3 need you".
export function needWords(n: number): string {
  return `${n} ${n === 1 ? 'needs' : 'need'} you`;
}

// What needs an admin in `area`, as its status: its danger and warn findings
// (Operations also holds the firing alarms), danger or an alarm the worse;
// null when nothing does or nothing was read.
export function areaAttention(attention: Attention | null, area: AdminArea['id']): AreaStatus | null {
  if (!attention) return null;
  const findings = attention.findings.filter((f) => f.level !== 'info' && areaOfLink(f.link) === area);
  const alarms = area === 'operations' ? attention.alarms.length : 0;
  const count = findings.length + alarms;
  if (count === 0) return null;
  const bad = alarms > 0 || findings.some((f) => f.level === 'danger');
  return { tone: bad ? 'bad' : 'warn', label: needWords(count) };
}

// "1 payment past due", "3 jobs failed": a count and its noun.
export function counted(n: number, one: string, many: string): string {
  return `${n} ${n === 1 ? one : many}`;
}
