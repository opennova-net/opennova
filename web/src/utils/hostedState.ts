// A hosted server's states (hosted_repository.h State) for display: their
// names (the service's own, and the owner's plainer ones), what each means to
// the owner, the start's progression, and a badge; then where a start stands
// within them (its phase, hosted/model/server_phase.h) and why a server's last run
// ended, in its owner's words. An owner never sees a raw log or a machine's
// figures: the phase and the server's own last line are all.

import { timeMs } from './format';
import type { HostedServer, HostedState, ServerPhase, StopLimit, StoppedBecause } from '../types/hostedServer';

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

// "1.2 of 2.3 GB", "300 of 600 MB": two byte counts in the larger one's
// decimal unit, one decimal under ten.
export function bytesOf(done: number, total: number): string {
  const units: [number, string][] = [
    [1e12, 'TB'],
    [1e9, 'GB'],
    [1e6, 'MB'],
    [1e3, 'kB'],
  ];
  const [size, unit] = units.find(([u]) => total >= u) ?? [1, 'B'];
  const shown = (v: number) => {
    const n = v / size;
    return String(n < 10 ? Math.round(n * 10) / 10 : Math.round(n));
  };
  return `${shown(Math.min(done, total))} of ${shown(total)} ${unit}`;
}

// What the fleet does for a starting server, in its owner's words; '' for the
// states themselves (running, stopping, stopped, failed), whose note says it.
export function phaseText(phase: ServerPhase): string {
  switch (phase.name) {
    case 'queued':
      return 'In line: the fleet picks it up in a moment.';
    case 'restarting': {
      const after = phase.cause === 'timeout' ? 'after it did not come up in time' : 'after a crash';
      return phase.restart !== undefined && phase.restart_limit !== undefined
        ? `Restarting ${after} (restart ${phase.restart} of ${phase.restart_limit}).`
        : `Restarting ${after}.`;
    }
    case 'moving':
      return 'Moving to another machine. This is not a restart and does not count toward the restart limit.';
    case 'data_unavailable':
      return 'The game data could not be fetched; the service is retrying.';
    case 'launches_held':
      return 'Waiting for a machine: new machines are held for a moment.';
    case 'launching_machine':
      return 'Starting a machine for it.';
    case 'waiting_for_slot':
      return 'Waiting for a place on a machine.';
    case 'waiting_for_address':
      return 'Waiting for your static address to reach its machine.';
    case 'downloading_data':
      return phase.bytes_done !== undefined && phase.bytes_total !== undefined && phase.bytes_total > 0
        ? `Downloading game data: ${bytesOf(phase.bytes_done, phase.bytes_total)}.`
        : 'Downloading game data.';
    case 'downloading_server':
      return 'Downloading the game server.';
    case 'starting_server':
      return 'Starting the game server.';
    case 'signing_in':
      return 'Signing in to NovaWorld.';
    default:
      return '';
  }
}

// How long a phase usually lasts from the server's last change of state, in
// seconds; null for one that has no usual length (a hold, the states
// themselves). The game data's grows with its size.
export function expectedPhaseSeconds(phase: ServerPhase): number | null {
  switch (phase.name) {
    case 'queued':
    case 'signing_in':
      return 60;
    case 'restarting':
    case 'moving':
    case 'data_unavailable':
    case 'waiting_for_slot':
    case 'starting_server':
      return 120;
    case 'downloading_server':
      return 180;
    case 'launching_machine':
    case 'waiting_for_address':
      // A machine's launch; its static address's wait comes after it in the same state.
      return 300;
    case 'downloading_data':
      // A minute or two, and 20 MB a second.
      return phase.bytes_total ? 120 + phase.bytes_total / 20e6 : 300;
    default:
      return null;
  }
}

export const SLOW_PHASE_TEXT = 'This is taking longer than usual.';

// Whether a start has stood in its phase past twice its usual length.
export function phaseSlow(server: Pick<HostedServer, 'phase' | 'updated_at'>, now: number): boolean {
  if (!server.phase) return false;
  const expected = expectedPhaseSeconds(server.phase);
  const since = timeMs(server.updated_at);
  if (expected === null || Number.isNaN(since)) return false;
  return now - since > 2 * expected * 1000;
}

// The fleet's notice while launches are held (a refused launch's backoff).
export const LAUNCHES_HELD_NOTICE =
  'New machines are held for a moment after the cloud turned a launch down. A server waiting for a machine starts as soon as launches resume; running servers carry on.';

// Why a stopped or failed server's last run ended, in its owner's words (on an
// admin's page, `scope` admin, of its owner): who stopped it, and for a stop
// the hosting limits made, the limit it was past.
export function stoppedBecauseText(b: StoppedBecause, scope: 'owner' | 'admin'): string {
  const owner = scope === 'owner';
  switch (b.cause) {
    case 'crash':
      if (b.oom) return 'It crashed: it ran out of memory.';
      return b.exit_code !== null && b.exit_code !== 0 ? `It crashed (exit code ${b.exit_code}).` : 'It crashed.';
    case 'timeout':
      return 'It did not come up in time.';
    case 'fleet':
      return 'The fleet ended the machine it ran on.';
    case 'stop':
      return 'It was stopped.';
    case 'owner_stop':
      return owner ? 'You stopped it.' : 'Its owner stopped it.';
    case 'admin_stop':
      return 'An admin stopped it.';
    case 'update':
      return 'It was restarted onto newer game data.';
    case 'limits':
      return limitsText(b.limit, owner);
    case 'no_data':
      return 'Its game data is no longer offered, so it could not run.';
    case 'data_unavailable':
      return 'Its game data could not be fetched.';
    case 'link_lost':
      return 'The service lost its link to it.';
    default:
      return `It ended (${b.cause}).`;
  }
}

// A limits stop: the limit its server was past and how many its owner had
// then, or, for a stop that recorded none (an older one), what such a stop
// means.
function limitsText(limit: StopLimit | null, owner: boolean): string {
  const your = owner ? 'Your' : "Its owner's";
  if (limit) {
    const machines = `${your} dedicated machines went down to ${limit.count}`;
    switch (limit.kind) {
      case 'shared_servers':
        return `${your} shared servers went down to ${limit.count}, so it was stopped.`;
      case 'no_shared_server':
        return `${machines} and ${owner ? 'you' : 'they'} had no shared server free for it, so it was stopped.`;
      case 'no_shared_room':
        return `${machines} and the shared machines had no room for it, so it was stopped.`;
    }
  }
  return owner
    ? 'Your hosting limit no longer covers it (your support tier, dedicated machine or admin grant ended or changed), so it was stopped.'
    : "Its owner's hosting limit no longer covers it (their support tier, dedicated machine or admin grant ended or changed), so it was stopped.";
}
