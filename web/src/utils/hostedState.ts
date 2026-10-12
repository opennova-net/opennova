// A hosted server's states (hosted_repository.h State) for display: their
// names, what each means to the owner, the start's progression, and a badge.

import type { HostedState } from '../types/hostedServer';

export const STATE_LABELS: Record<HostedState, string> = {
  requested: 'Requested',
  provisioning: 'Provisioning',
  booting: 'Booting',
  listing: 'Listing',
  running: 'Running',
  stopping: 'Stopping',
  stopped: 'Stopped',
  failed: 'Failed',
};

export const STATE_DESCRIPTIONS: Record<HostedState, string> = {
  requested: 'Your request is queued; the service picks it up within a few seconds.',
  provisioning: 'The service is preparing the server: its NovaWorld account, its files and its machine.',
  booting: 'The machine is starting the game server.',
  listing: 'The game server runs and is listing itself on NovaWorld.',
  running: 'Listed on NovaWorld: players can find it in the server browser and join.',
  stopping: 'Ending the match and shutting the machine down.',
  stopped: 'The server has stopped and its machine is gone.',
  failed: 'The server failed and its machine is gone.',
};

// A state's name, for any text the service sent ('' and an unknown state
// shown as they are).
export function stateLabel(state: string): string {
  return (STATE_LABELS as Record<string, string>)[state] ?? state;
}

// The steps a start goes through, in order, up to running.
export const START_STEPS: readonly HostedState[] = ['requested', 'provisioning', 'booting', 'listing', 'running'];

// Any state but stopped and failed (the backend's `live`).
export function isLive(state: HostedState): boolean {
  return state !== 'stopped' && state !== 'failed';
}

// A live state the supervisor is moving the server out of on its own: the
// start's steps before running, and a stop.
export function isTransitioning(state: HostedState): boolean {
  return isLive(state) && state !== 'running';
}

export function stateBadgeClass(state: HostedState): string {
  const base = 'inline-flex items-center rounded-control px-2 py-0.5 text-xs font-semibold uppercase tracking-wider';
  switch (state) {
    case 'running':
      return `${base} bg-online/15 text-online`;
    case 'failed':
      return `${base} bg-danger/15 text-danger`;
    case 'stopping':
    case 'requested':
    case 'provisioning':
    case 'booting':
    case 'listing':
      return `${base} bg-warn/15 text-warn`;
    case 'stopped':
      return `${base} bg-hover text-ink-muted`;
  }
}
