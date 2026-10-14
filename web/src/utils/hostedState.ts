// A hosted server's states (hosted_repository.h State) for display: their
// names (the service's own, and the owner's plainer ones), what each means to
// the owner, the start's progression, and a badge.

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
  requested: 'Your server is in line to start.',
  provisioning: 'Your server is being set up.',
  booting: 'Your server is starting up.',
  listing: 'Your server is almost ready.',
  running: 'Players can find it in the server browser and join.',
  stopping: 'Your server is shutting down.',
  stopped: 'Your server has stopped.',
  failed: 'Something went wrong, so your server was shut down.',
};

// The owner's names: the start's middle states are all "Starting".
export const OWNER_STATE_LABELS: Record<HostedState, string> = {
  requested: 'Requested',
  provisioning: 'Starting',
  booting: 'Starting',
  listing: 'Starting',
  running: 'Running',
  stopping: 'Stopping',
  stopped: 'Stopped',
  failed: 'Failed',
};

// A state's name, for any text the service sent ('' and an unknown state
// shown as they are).
export function stateLabel(state: string): string {
  return (STATE_LABELS as Record<string, string>)[state] ?? state;
}

// A state's name as its owner sees it, for any text the service sent.
export function ownerStateLabel(state: string): string {
  return (OWNER_STATE_LABELS as Record<string, string>)[state] ?? state;
}

// What a state means to the owner; empty for a state this site does not know.
export function stateDescription(state: string): string {
  return (STATE_DESCRIPTIONS as Record<string, string>)[state] ?? '';
}

// The steps a start goes through, in order, up to running.
export const START_STEPS: readonly HostedState[] = ['requested', 'provisioning', 'booting', 'listing', 'running'];

// The same steps as the owner sees them, each with the states it covers.
export const OWNER_START_STEPS: readonly { label: string; states: readonly HostedState[] }[] = [
  { label: 'Requested', states: ['requested'] },
  { label: 'Starting', states: ['provisioning', 'booting', 'listing'] },
  { label: 'Running', states: ['running'] },
];

// The owner's step a state is on, or -1 once it is stopping or has ended.
export function ownerStepIndex(state: HostedState): number {
  return OWNER_START_STEPS.findIndex((step) => step.states.includes(state));
}

// Any state but stopped and failed (the backend's `live`).
export function isLive(state: HostedState): boolean {
  return state !== 'stopped' && state !== 'failed';
}

// A live state the supervisor is moving the server out of on its own: the
// start's steps before running, and a stop.
export function isTransitioning(state: HostedState): boolean {
  return isLive(state) && state !== 'running';
}

// A state this site does not know gets the ended look.
export function stateBadgeClass(state: string): string {
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
    default:
      return `${base} bg-hover text-ink-muted`;
  }
}
