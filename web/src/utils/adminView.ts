// Labels the admin pages share: account statuses, support tiers and admin
// grants, the sold capacity, machines, and the lobby instances, each in words.

import type { EntitlementSource, MachineKind } from '../types/hostedServer';
import type { Tone } from './tone';
import type { SoldCapacity } from '../types/users';

// "Active", "Banned", "Restricted": a players.account_status; one stored
// empty reads as active.
export function accountStatusLabel(status: string): string {
  const s = status || 'active';
  return s.charAt(0).toUpperCase() + s.slice(1).replace(/_/g, ' ');
}

export function isActiveAccount(status: string): boolean {
  return (status || 'active') === 'active';
}

// Where an account's hosted servers come from, each source it has: the one
// that gives its servers on shared machines first, then the dedicated
// machines it pays for. "Tier 2", "Admin grant of 9 shared + 2 dedicated",
// "Admin grant of 9 shared, Tier 2" (a tier still billing under a bigger
// grant), "Tier 2, Admin grant of 2 shared", "Tier 2, 1 dedicated",
// "1 dedicated", "None".
export function entitlementLabel(e: {
  source: EntitlementSource;
  tier: number | null;
  comp_servers: number;
  comp_own_machines: number;
  own_machines: number;
}): string {
  const granted = [
    ...(e.comp_servers > 0 ? [`${e.comp_servers} shared`] : []),
    ...(e.comp_own_machines > 0 ? [`${e.comp_own_machines} dedicated`] : []),
  ];
  const grant = granted.length > 0 ? `Admin grant of ${granted.join(' + ')}` : '';
  const tier = e.tier !== null ? `Tier ${e.tier}` : '';
  // The paid dedicated machines; "paid" only beside the grant's own, so the two never read alike.
  const paid = e.own_machines - e.comp_own_machines;
  const paidLabel = e.comp_own_machines > 0 ? `${paid} paid dedicated` : `${paid} dedicated`;
  const parts = [...(e.source === 'tier' ? [tier, grant] : [grant, tier]), paid > 0 ? paidLabel : ''];
  const named = parts.filter((p) => p !== '');
  return named.length > 0 ? named.join(', ') : 'None';
}

// A fleet view's owner's hosting: "Admin grant, Tier 2", "Admin grant",
// "Tier 2", "No tier".
export function ownerHostingLabel(owner: { tier: number | null; granted: boolean }): string {
  const parts = [...(owner.granted ? ['Admin grant'] : []), ...(owner.tier !== null ? [`Tier ${owner.tier}`] : [])];
  return parts.length > 0 ? parts.join(', ') : 'No tier';
}

// Where an entitlement's servers run, as the sold capacity counts them: its
// servers on shared machines, and one on each dedicated machine. "9 on
// shared machines, 2 on dedicated machines", "3 on shared machines",
// "1 on a dedicated machine", "None".
export function serverPlacesLabel(e: { shared_servers: number; own_machines: number }): string {
  const own = e.own_machines === 1 ? '1 on a dedicated machine' : `${e.own_machines} on dedicated machines`;
  const places = [
    ...(e.shared_servers > 0 ? [`${e.shared_servers} on shared machines`] : []),
    ...(e.own_machines > 0 ? [own] : []),
  ];
  return places.length > 0 ? places.join(', ') : 'None';
}

// The service's sold capacity against what the fleet sells, item by item:
// "Sold: 9 of 12 shared places, 2 of 2 dedicated machines, 0 of 1 static
// addresses."
export function soldCapacityText(c: SoldCapacity): string {
  return (
    `Sold: ${c.shared.sold} of ${c.shared.sellable} shared places, ` +
    `${c.own_machines.sold} of ${c.own_machines.sellable} dedicated machines, ` +
    `${c.addresses.sold} of ${c.addresses.sellable} static addresses.`
  );
}

export function machineKindLabel(kind: MachineKind | string): string {
  return kind === 'own' ? 'Dedicated machine' : kind === 'shared' ? 'Shared machine' : kind;
}

const MACHINE_STATES: Record<string, string> = {
  launching: 'Launching',
  booting: 'Booting',
  ready: 'Ready',
  draining: 'Draining',
  ending: 'Ending',
  ended: 'Ended',
  failed: 'Failed',
  lost: 'Lost (no reports)',
};

export function machineStateLabel(state: string): string {
  return MACHINE_STATES[state] ?? state;
}

// A task id's first 12 characters (an ECS task id's, a local container's id
// whole), as the program page names its tasks.
export function shortTaskId(id: string): string {
  return id.slice(0, 12);
}

// The page of the lobby program's usage, the task `taskId` picked when it
// names one.
export function lobbyUsageLink(taskId: string): { path: string; query?: { task: string } } {
  const path = '/admin/programs/lobby';
  return taskId ? { path, query: { task: taskId } } : { path };
}

// A lobby instance's heartbeat moves every 5 s; one seen within this long
// runs (lobby_repository.h kInstanceLiveSeconds).
export const LOBBY_LIVE_MS = 15_000;

// A lobby instance's state in words and a tone: not seen within LOBBY_LIVE_MS
// (its process may be gone), taking clients, or draining.
export function lobbyStateLabel(lobby: { state: string; last_seen_ms: number }, now: number): { label: string; tone: Tone } {
  const seen = sessionTime(lobby.last_seen_ms, now);
  if (seen === null || now - seen > LOBBY_LIVE_MS) return { label: 'Not seen', tone: 'bad' };
  return lobby.state === 'active' ? { label: 'Taking clients', tone: 'ok' } : { label: 'Draining', tone: 'warn' };
}

// The oldest a session time can be: a year (a session ends after four minutes
// without a keepalive, and none has lived a year).
export const SESSION_TIME_MAX_AGE_MS = 365 * 86_400_000;
// How far ahead of the page's clock a time may read (the two clocks differ).
export const SESSION_TIME_MAX_AHEAD_MS = 5 * 60_000;

// A session time the service sent (epoch ms), or null when it cannot be one:
// none (0), older than SESSION_TIME_MAX_AGE_MS or further ahead of `now` than
// SESSION_TIME_MAX_AHEAD_MS. The page shows such a time as unknown, never as a
// day count (a time read on the wrong clock once showed as 20,741 days ago).
export function sessionTime(ms: number, now: number): number | null {
  if (!Number.isFinite(ms) || ms <= 0) return null;
  if (now - ms > SESSION_TIME_MAX_AGE_MS || ms - now > SESSION_TIME_MAX_AHEAD_MS) return null;
  return ms;
}
