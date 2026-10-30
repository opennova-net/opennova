// A fleet machine's words for the Machines tab's cards and the machine page:
// how long ago its agent reported (late past 30 s), what it uses against the
// thresholds the health check flags (fleet_health.h's machine pressure), how
// it differs from the release, its slots, its trees' progress and its events.

import { machineStateLabel } from './adminView';
import { formatAge, formatByteRate, formatBytes, formatDuration, formatMemory, formatPercent, timeMs } from './format';
import { stateLabel } from './hostedState';
import { titleName } from './games';
import type { Tone } from './tone';
import type { FleetMachine, FleetServer, MachineEvent, MachineStats, MachineTree, SlotHolder } from '../types/hostedServer';

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
    return { text: 'No report yet', late: m.state !== 'launching' && m.state !== 'booting' };
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
  out.push({ label: 'Network', value: net.length ? net.join(', ') : '—', flag: '' });
  return out;
}

// What differs from the release (outdated), each in words.
export function outdatedReasons(m: Pick<FleetMachine, 'agent_image' | 'image_id' | 'release'>): string[] {
  const out: string[] = [];
  if (m.release.agent_image && m.agent_image && m.agent_image !== m.release.agent_image) out.push('hostd image');
  if (m.release.image_id && m.image_id && m.image_id !== m.release.image_id) out.push(`machine image ${m.image_id}`);
  return out;
}

// What is wrong with a live machine, each in a few words, `bad` for what keeps
// it from taking servers: lost and held by the mass-lost brake, a protocol
// mismatch held, older than the release.
export function machineFlags(
  m: Pick<FleetMachine, 'state' | 'lost_held' | 'protocol_mismatch_since' | 'agent_protocol' | 'outdated' | 'agent_image' | 'image_id' | 'release'>,
  now: number,
): { text: string; bad: boolean }[] {
  if (!machineLive(m.state)) return [];
  const out: { text: string; bad: boolean }[] = [];
  if (m.lost_held) out.push({ text: 'Lost, held by the brake', bad: true });
  if (m.protocol_mismatch_since) {
    const held = formatDuration(Math.max(0, now - timeMs(m.protocol_mismatch_since)));
    out.push({ text: `Agent protocol ${m.agent_protocol ?? 'unknown'} mismatch, held ${held}`, bad: true });
  }
  if (m.outdated === true) {
    const why = outdatedReasons(m);
    out.push({ text: `Older than the release${why.length ? `: ${why.join(', ')}` : ''}`, bad: false });
  }
  return out;
}

// A machine's state as a badge's tone.
export function machineTone(state: string): Tone {
  if (state === 'failed' || state === 'lost') return 'bad';
  if (state === 'ready') return 'ok';
  if (state === 'ended') return 'muted';
  return 'warn';
}

// A fact against the release's: the same, another, or not comparable.
export function releaseMatch(own: string | null, release: string | null): { text: string; tone: Tone } {
  if (!release || !own) return { text: '', tone: 'muted' };
  return own === release ? { text: "Release's", tone: 'ok' } : { text: 'Not the release', tone: 'warn' };
}

// A game's short name for a slot cell ("JO", "DFX2"): its product's key, else
// its title's.
export function gameShortName(server: Pick<FleetServer, 'product'>): string {
  const product = server.product;
  if (!product) return '';
  return product.key || (product.title ? titleName(product.title) : '');
}

export interface SlotCell {
  slot: number;
  holder: SlotHolder | null;
  // The server's name (its markup kept), its game's short name, and its
  // players "5/32" ('' when not listed).
  name: string;
  game: string;
  players: string;
  // Its state in a word ("Running", "Late", "Starting"), and that word's tone.
  word: string;
  tone: Tone;
}

// One slot as a cell: free, or the server holding it (the machine's slot
// holder), named from the holder where it names itself (the machine's page)
// or from the fleet's server rows; "Late" when its last minute had late ticks
// (it could not keep its 62.5 Hz).
export function slotCell(slot: number, holder: SlotHolder | null, servers: readonly FleetServer[]): SlotCell {
  if (!holder) return { slot, holder, name: '', game: '', players: '', word: 'Free', tone: 'muted' };
  const server = servers.find((s) => s.id === holder.server_id) ?? null;
  const name = holder.name || server?.listing?.server_name || server?.settings.game_name || `Server ${holder.server_id}`;
  const game = server ? gameShortName(server) : '';
  const max = server?.listing?.max_players ?? server?.settings.max_players ?? null;
  const players = server?.players != null && max !== null ? `${server.players}/${max}` : '';
  const late = ((holder.tick ?? server?.tick)?.late ?? 0) > 0;
  if (holder.state === 'running') return { slot, holder, name, game, players, word: late ? 'Late' : 'Running', tone: late ? 'warn' : 'ok' };
  if (holder.state === 'failed') return { slot, holder, name, game, players, word: 'Failed', tone: 'bad' };
  const word = holder.state === 'stopping' ? 'Stopping' : holder.state === 'stopped' ? stateLabel(holder.state) : 'Starting';
  return { slot, holder, name, game, players, word, tone: 'warn' };
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
      return `Fetching: ${of}${tries}${t.error ? `; last try failed: ${t.error}` : ''}`;
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
