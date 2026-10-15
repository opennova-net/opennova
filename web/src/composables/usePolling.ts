// One read a page repeats on a cadence it picks from what it read, aware of the
// tab: a hidden tab skips its polls and reads at once when shown again (unless
// a poll is due anyway); a failed poll is retried from 5 s, doubling to a
// minute; an asked-for read takes over from one in flight, while a poll that
// finds one in flight leaves it be. Everything stops when the component
// unmounts, and the read in flight is aborted.
//
// hostedCadence() is the hosted servers' cadence: every 3 s while any of them
// changes state on its own (a start's steps, a stop), else from 10 s doubling
// to a minute while any runs, and no poll once none is live.

import { onMounted, onUnmounted, ref } from 'vue';
import type { HostedServer } from '../types/hostedServer';
import { isLive, isTransitioning } from '../utils/hostedState';

export const RETRY_POLL_FIRST_MS = 5_000;
export const RETRY_POLL_MAX_MS = 60_000;

export const TRANSITION_POLL_MS = 3_000;
export const RUNNING_POLL_FIRST_MS = 10_000;
export const RUNNING_POLL_MAX_MS = 60_000;

export interface PollingOptions {
  // One read. `background`: a poll, which shows nothing when it fails (an
  // asked-for read shows its failure). Resolves true when it got an answer,
  // false when it failed; an aborted read's answer is ignored.
  read: (signal: AbortSignal, background: boolean) => Promise<boolean>;
  // After an answer: the delay to the next poll in ms, or null for none
  // (nothing changes on its own; the tab shown again still reads).
  next: () => number | null;
  // Read on mount (the default), or wait for the first refresh().
  immediate?: boolean;
}

export function usePolling(options: PollingOptions) {
  // An asked-for read is in flight.
  const loading = ref(false);
  let timer: ReturnType<typeof setTimeout> | undefined;
  let inFlight: AbortController | null = null;
  let retryDelay = RETRY_POLL_FIRST_MS;
  let unmounted = false;

  function clearTimer() {
    if (timer) clearTimeout(timer);
    timer = undefined;
  }

  function schedule(delay: number | null) {
    clearTimer();
    if (unmounted || delay === null) return;
    timer = setTimeout(() => {
      timer = undefined;
      // A hidden tab waits for visibilitychange, which reads at once.
      if (typeof document !== 'undefined' && document.hidden) return;
      void run(true);
    }, delay);
  }

  async function run(background: boolean): Promise<void> {
    if (unmounted) return;
    if (inFlight) {
      if (background) return;
      inFlight.abort();
    }
    const controller = new AbortController();
    inFlight = controller;
    if (!background) loading.value = true;
    let answered = false;
    try {
      answered = await options.read(controller.signal, background);
    } finally {
      if (inFlight === controller) {
        inFlight = null;
        loading.value = false;
      }
    }
    // A newer read took over, or the page has gone.
    if (controller.signal.aborted || unmounted) return;
    if (answered) {
      retryDelay = RETRY_POLL_FIRST_MS;
      schedule(options.next());
    } else {
      const delay = retryDelay;
      retryDelay = Math.min(retryDelay * 2, RETRY_POLL_MAX_MS);
      schedule(delay);
    }
  }

  // An asked-for read: it shows its failure, and takes over from a read in
  // flight.
  function refresh(): Promise<void> {
    return run(false);
  }

  // A quiet read now, after the page changed something itself: a read that
  // started before the change would answer with the state from before it, so
  // it is dropped and this one is in charge.
  function readAgain(): Promise<void> {
    inFlight?.abort();
    inFlight = null;
    loading.value = false;
    return run(true);
  }

  // A quiet read now unless one is in flight.
  function poke(): Promise<void> {
    return run(true);
  }

  function onVisibility() {
    if (!document.hidden && !timer) void run(true);
  }

  onMounted(() => {
    if (options.immediate !== false) void run(false);
    document.addEventListener('visibilitychange', onVisibility);
  });

  onUnmounted(() => {
    unmounted = true;
    clearTimer();
    document.removeEventListener('visibilitychange', onVisibility);
    inFlight?.abort();
    inFlight = null;
  });

  return { loading, refresh, readAgain, poke };
}

// The hosted servers' cadence for usePolling's `next`, one per page: the
// running backoff starts over whenever the live servers or their states
// change.
export function hostedCadence() {
  let signature = '';
  let runningDelay = RUNNING_POLL_FIRST_MS;
  return (servers: readonly Pick<HostedServer, 'id' | 'state'>[]): number | null => {
    const live = servers.filter((s) => isLive(s.state));
    const now = live.map((s) => `${s.id}:${s.state}`).join(',');
    if (now !== signature) {
      signature = now;
      runningDelay = RUNNING_POLL_FIRST_MS;
    }
    if (live.length === 0) return null;
    if (live.some((s) => isTransitioning(s.state))) return TRANSITION_POLL_MS;
    const delay = runningDelay;
    runningDelay = Math.min(runningDelay * 2, RUNNING_POLL_MAX_MS);
    return delay;
  };
}
