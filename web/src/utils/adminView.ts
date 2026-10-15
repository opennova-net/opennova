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

// "Tier 2", "Admin grant of 3", "None".
export function entitlementLabel(e: { source: EntitlementSource; tier: number | null; max_servers: number }): string {
  if (e.source === 'tier' && e.tier !== null) return `Tier ${e.tier}`;
  if (e.source === 'comp') return `Admin grant of ${e.max_servers}`;
  if (e.tier !== null) return `Tier ${e.tier}`;
  return 'None';
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
