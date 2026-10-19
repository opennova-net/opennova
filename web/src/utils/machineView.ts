// A fleet machine's words for the Fleet page's cards and the machine page
// (components/MachineCard.vue, pages/admin/MachineDetailSection.vue): how
// long ago its agent reported (late past 30 s), what it uses against the
// thresholds the health check flags (fleet_health.h's machine pressure), how
// it differs from the release, its trees' progress and its events. State is
// said in words as well as colour.

import { machineStateLabel } from './adminView';
import { formatAge, formatByteRate, formatBytes, formatDuration, formatMemory, formatPercent, timeMs } from './format';
import type { Tone } from './infraView';
import type { FleetMachine, MachineEvent, MachineStats, MachineTree } from '../types/hostedServer';

// A report older than this is late: the agent syncs every few seconds, and the
// fleet calls a machine lost after 60 s of silence.
export const REPORT_LATE_MS = 30_000;

// The health check's machine pressure: CPU over 85 %, memory over 90 % used,
// disk under 15 % free.
export const CPU_HIGH_PERCENT = 85;
export const MEMORY_HIGH_FRACTION = 0.9;
export const DISK_LOW_FRACTION = 0.15;

// Any state but ended and failed.
export function machineLive(state: string): boolean {
  return state !== 'ended' && state !== 'failed';
}

// "Last report 4 s ago", late (and so red) past 30 s; null for a machine that
// no longer exists.
export function reportAge(m: Pick<FleetMachine, 'state' | 'last_report_at'>, now: number): { text: string; late: boolean } | null {
  if (!machineLive(m.state)) return null;
  if (!m.last_report_at) {
    // A launching or booting machine's agent has not started yet.
    const waiting = m.state === 'launching' || m.state === 'booting';
    return { text: waiting ? 'No report yet: its agent is starting' : 'No report yet', late: !waiting };
  }
  const t = timeMs(m.last_report_at);
  if (Number.isNaN(t)) return { text: `Last report ${m.last_report_at}`, late: false };
  const late = now - t > REPORT_LATE_MS;
  return { text: `Last report ${formatAge(t, now)}${late ? ': late' : ''}`, late };
}

export interface MachineFigure {
  label: string;
  value: string;
  // Past the health check's threshold, and the word that says so.
  flag: string;
}

// What it uses now, a figure each: CPU, memory, disk, network; empty before
// its first report.
export function machineFigures(stats: MachineStats | null): MachineFigure[] {
  if (!stats) return [];
  const out: MachineFigure[] = [];
  if (stats.cpu_percent !== null) {
    out.push({ label: 'CPU', value: formatPercent(stats.cpu_percent), flag: stats.cpu_percent > CPU_HIGH_PERCENT ? 'high' : '' });
  }
  if (stats.memory_used_bytes !== null) {
    const total = stats.memory_total_bytes;
    out.push({
      label: 'Memory',
      value: total !== null ? `${formatMemory(stats.memory_used_bytes)} of ${formatMemory(total)}` : formatMemory(stats.memory_used_bytes),
      flag: total !== null && total > 0 && stats.memory_used_bytes / total > MEMORY_HIGH_FRACTION ? 'high' : '',
    });
  }
  if (stats.disk_free_bytes !== null) {
    const total = stats.disk_total_bytes;
    out.push({
      label: 'Disk',
      value: total !== null ? `${formatBytes(stats.disk_free_bytes)} free of ${formatBytes(total)}` : `${formatBytes(stats.disk_free_bytes)} free`,
      flag: total !== null && total > 0 && stats.disk_free_bytes / total < DISK_LOW_FRACTION ? 'low' : '',
    });
  }
  const net: string[] = [];
  if (stats.net_in_bps !== null) net.push(`${formatByteRate(stats.net_in_bps)} in`);
  if (stats.net_out_bps !== null) net.push(`${formatByteRate(stats.net_out_bps)} out`);
  out.push({ label: 'Network', value: net.length ? net.join(', ') : 'Not measured yet', flag: '' });
  return out;
}

// What differs from the release (outdated), each in words.
export function outdatedReasons(m: Pick<FleetMachine, 'agent_image' | 'image_id' | 'release'>): string[] {
  const out: string[] = [];
  if (m.release.agent_image && m.agent_image && m.agent_image !== m.release.agent_image) {
    out.push("its hostd image is not the release's");
  }
  if (m.release.image_id && m.image_id && m.image_id !== m.release.image_id) {
    out.push(`its machine image ${m.image_id} is not the release's ${m.release.image_id}`);
  }
  return out;
}

// What is wrong with a live machine, each in words, `bad` for what keeps it
// from taking servers: lost and held by the mass-lost brake, a protocol
// mismatch held, older than the release.
export function machineFlags(
  m: Pick<FleetMachine, 'state' | 'lost_held' | 'protocol_mismatch_since' | 'agent_protocol' | 'outdated' | 'agent_image' | 'image_id' | 'release'>,
  now: number,
): { text: string; bad: boolean }[] {
  if (!machineLive(m.state)) return [];
  const out: { text: string; bad: boolean }[] = [];
  if (m.lost_held) {
    out.push({ text: 'Lost and held by the brake: it is not ended until it reports again or an admin ends it.', bad: true });
  }
  if (m.protocol_mismatch_since) {
    const held = formatDuration(Math.max(0, now - timeMs(m.protocol_mismatch_since)));
    out.push({
      text: `Protocol mismatch, held for ${held}: its hostd speaks protocol ${m.agent_protocol ?? 'unknown'}, so it takes no server; it drains after 10 minutes.`,
      bad: true,
    });
  }
  if (m.outdated === true) {
    const why = outdatedReasons(m);
    out.push({ text: `Older than the release${why.length ? `: ${why.join('; ')}` : ''}.`, bad: false });
  }
  return out;
}

// A machine's state as a badge's tone.
export function machineTone(state: string): Tone {
  if (state === 'failed' || state === 'lost') return 'bad';
  if (state === 'ready') return 'ok';
  if (state === 'ended') return 'idle';
  return 'warn';
}

// "Ready, 1.2 GB", "Fetching: 600 MB of 1.2 GB (50 %), try 2", "Failed after 3
// tries: <why>".
export function treeProgress(t: MachineTree): string {
  const tries = t.tries > 1 ? `, try ${t.tries}` : '';
  switch (t.state) {
    case 'ready':
      return `Ready, ${formatBytes(t.bytes)}`;
    case 'fetching': {
      const of = t.total_bytes !== null && t.total_bytes > 0
        ? `${formatBytes(t.bytes)} of ${formatBytes(t.total_bytes)} (${formatPercent(Math.min(100, (t.bytes / t.total_bytes) * 100))})`
        : `${formatBytes(t.bytes)} so far`;
      return `Fetching: ${of}${tries}${t.error ? `; the last try failed: ${t.error}` : ''}`;
    }
    case 'failed':
      return `Failed after ${t.tries} ${t.tries === 1 ? 'try' : 'tries'}${t.error ? `: ${t.error}` : ''}`;
    default:
      return t.state;
  }
}

// The fraction of a fetching tree's bytes done, for a progress bar; null when
// its total is not known.
export function treeFraction(t: MachineTree): number | null {
  if (t.state === 'ready') return 1;
  if (t.total_bytes === null || t.total_bytes <= 0) return null;
  return Math.min(1, t.bytes / t.total_bytes);
}

// "Made: launching", "Booting to ready: first report", "log stream hostd/..."
export function eventText(e: MachineEvent): string {
  if (e.from === null) return `Made: ${machineStateLabel(e.to).toLowerCase()}${e.detail ? ` (${e.detail})` : ''}`;
  if (e.from === e.to) return e.detail || machineStateLabel(e.to);
  return `${machineStateLabel(e.from)} to ${machineStateLabel(e.to).toLowerCase()}${e.detail ? `: ${e.detail}` : ''}`;
}
