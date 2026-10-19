// Labels the admin pages share: account statuses, support tiers and admin
// grants, the sold capacity, machines, NovaWorld sessions, the titles they
// speak, their servers and their lobby instances, each in words.

import type { ServerKind } from '../types/admin';
import type { EntitlementSource, MachineKind } from '../types/hostedServer';
import type { Tone } from './infraView';
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

// A NovaWorld session's lifecycle (the connection registry's states).
const SESSION_STATES: Record<string, string> = {
  handshaking: 'Signing in',
  active: 'Signed in',
  closing: 'Signing out',
};

export function sessionStateLabel(state: string): string {
  return SESSION_STATES[state] ?? state;
}

// The titles' own protocol names a client hello carries (engine/net/title:
// each title's PN). NOVAWORLDUDP, the NovaWorld session every game client
// opens whatever its game, names none.
const PROTOCOL_NAMES: Record<string, string> = {
  JOINTOPERATIONS: 'Joint Operations',
  DELTAFORCEXTREME2: 'Delta Force Xtreme 2',
};

// The game a session's protocol name says, or null when it names none.
export function protocolTitle(pn: string): string | null {
  return PROTOCOL_NAMES[pn.toUpperCase()] ?? null;
}

// The game a session speaks, in words: its games row's name (the service's
// read of its listing or its game tag), else the title its protocol name
// says; null when neither names one.
export function sessionGame(s: { title: { display_name: string } | null; pn: string }): string | null {
  return s.title?.display_name ?? protocolTitle(s.pn);
}

// What a listing is, in words.
const SERVER_KINDS: Record<ServerKind, string> = {
  hosted: 'Hosted server',
  owned: "A player's own",
  anonymous: 'Anonymous',
};

export function serverKindLabel(kind: ServerKind): string {
  return SERVER_KINDS[kind];
}

// Retail's Serve Only flag in words: a Serve Only host runs the server alone,
// a listen host plays on it too; null while it is not listed.
export function serverRoleLabel(dedicated: boolean | null): string | null {
  if (dedicated === null) return null;
  return dedicated ? 'Serve Only' : 'Listen (plays too)';
}

// A task id's first 12 characters (an ECS task id's, a local container's id
// whole), as the program page names its tasks.
export function shortTaskId(id: string): string {
  return id.slice(0, 12);
}

// The page of the lobby program's usage, the task `taskId` picked when it
// names one.
export function lobbyUsageLink(taskId: string): { path: string; query?: { task: string } } {
  const path = '/admin/infrastructure/programs/lobby';
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

// How the sessions table orders its rows: by session id, account name, when
// each connected or when it was last heard from.
export type SessionSortKey = 'id' | 'account' | 'connected' | 'last_heard';

interface SortableSession {
  id: number;
  created_ms: number;
  last_seen_ms: number;
  account: { username: string } | null;
}

// `sessions` ordered by `key`, ascending or not; a session with no account, or
// a time it cannot have, goes last either way, and ties keep the id order.
export function sortSessions<T extends SortableSession>(
  sessions: readonly T[],
  key: SessionSortKey,
  ascending: boolean,
  now: number,
): T[] {
  const value = (s: T): number | string | null => {
    switch (key) {
      case 'id':
        return s.id;
      case 'account':
        return s.account ? s.account.username.toLowerCase() : null;
      case 'connected':
        return sessionTime(s.created_ms, now);
      case 'last_heard':
        return sessionTime(s.last_seen_ms, now);
    }
  };
  return [...sessions].sort((a, b) => {
    const va = value(a);
    const vb = value(b);
    if (va === null || vb === null) {
      if (va === vb) return a.id - b.id;
      return va === null ? 1 : -1;
    }
    const order = va < vb ? -1 : va > vb ? 1 : 0;
    if (order === 0) return a.id - b.id;
    return ascending ? order : -order;
  });
}
