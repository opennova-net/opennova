// Labels the admin pages share: account statuses, support tiers, machines,
// NovaWorld sessions and the titles they speak, each in words.

import type { EntitlementSource, MachineKind } from '../types/hostedServer';

// "Active", "Banned", "Restricted": a players.account_status; one stored
// empty reads as active.
export function accountStatusLabel(status: string): string {
  const s = status || 'active';
  return s.charAt(0).toUpperCase() + s.slice(1).replace(/_/g, ' ');
}

export function isActiveAccount(status: string): boolean {
  return (status || 'active') === 'active';
}

// Where an account's hosted servers come from, each source it has, the one
// that gives its servers first: "Tier 2", "Admin grant of 3", "Admin grant of
// 11, Tier 2" (a tier still billing under a bigger grant), "Tier 2, Admin
// grant of 2", "None".
export function entitlementLabel(e: { source: EntitlementSource; tier: number | null; comp_servers: number }): string {
  const grant = e.comp_servers > 0 ? `Admin grant of ${e.comp_servers}` : '';
  const tier = e.tier !== null ? `Tier ${e.tier}` : '';
  const parts = (e.source === 'tier' ? [tier, grant] : [grant, tier]).filter((p) => p !== '');
  return parts.length > 0 ? parts.join(', ') : 'None';
}

// A fleet view's owner's hosting: "Admin grant, Tier 2", "Admin grant",
// "Tier 2", "No tier".
export function ownerHostingLabel(owner: { tier: number | null; granted: boolean }): string {
  const parts = [...(owner.granted ? ['Admin grant'] : []), ...(owner.tier !== null ? [`Tier ${owner.tier}`] : [])];
  return parts.length > 0 ? parts.join(', ') : 'No tier';
}

// Where an entitlement's servers run, as the sold capacity counts them: an own
// machine takes one of them off the shared machines and adds none.
// "9 on shared machines, 2 on own machines", "All on shared machines", "None".
export function serverPlacesLabel(e: { max_servers: number; own_machines: number }): string {
  if (e.max_servers <= 0) return 'None';
  const own = Math.min(e.own_machines, e.max_servers);
  const shared = e.max_servers - own;
  if (own === 0) return 'All on shared machines';
  if (shared === 0) return 'All on own machines';
  return `${shared} on shared machines, ${own} on own machine${own === 1 ? '' : 's'}`;
}

export function machineKindLabel(kind: MachineKind | string): string {
  return kind === 'own' ? 'Own machine' : kind === 'shared' ? 'Shared machine' : kind;
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

// The protocol names a client hello carries (engine/net/title: each title's
// PN; NOVAWORLDUDP is the NovaWorld session every game client opens, whatever
// its game).
const PROTOCOL_NAMES: Record<string, string> = {
  NOVAWORLDUDP: 'NovaWorld lobby (any game)',
  JOINTOPERATIONS: 'Joint Operations',
  DELTAFORCEXTREME2: 'Delta Force Xtreme 2',
};

// The game a session's protocol name says, or null when it names none this
// site knows (the page then shows the raw name with a hint).
export function protocolTitle(pn: string): string | null {
  return PROTOCOL_NAMES[pn.toUpperCase()] ?? null;
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
