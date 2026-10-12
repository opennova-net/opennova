// One page's hosted-console calls, one at a time. The service runs at most one
// console request per account at once and answers a second 429 console_busy
// at once, with no wait (apps/novaworld_server/README.md, "The admission"), so
// every console call a page makes, its polls and its actions alike, goes
// through the page's one queue (composables/useConsoleQueue.ts), each starting
// only once the one before it has settled:
// - an action (a kick, an add, a chat line, ...) runs before every poll still
//   waiting, right after the call in flight;
// - a poll is dropped while the same poll (by its key) still waits or runs, so
//   a slow server never stacks them;
// - while the tab is hidden, and for good once the page has gone (dispose),
//   no poll runs: a waiting one settles as not run, and at dispose the poll in
//   flight is aborted. An action still runs: the person asked for it.

export type PollOutcome<T> = { ran: true; value: T } | { ran: false };

interface Task {
  kind: 'action' | 'poll';
  // A poll's key; '' for an action.
  key: string;
  // Runs the call and settles its caller's promise; never rejects.
  start: () => Promise<void>;
  // Settles a poll's caller as not run.
  skip: () => void;
}

export class ConsoleQueue {
  private readonly actions: Task[] = [];
  private readonly polls: Task[] = [];
  // The polls waiting or running.
  private readonly pollKeys = new Set<string>();
  private running = false;
  private disposed = false;
  private pollInFlight: AbortController | null = null;

  // Whether polls are held: the tab is hidden, or the page has gone.
  get pollsPaused(): boolean {
    return this.disposed || (typeof document !== 'undefined' && document.hidden);
  }

  // A call that changes the server: it runs after the call in flight and
  // before every waiting poll. Its promise settles as `run`'s does.
  action<T>(run: () => Promise<T>): Promise<T> {
    return new Promise<T>((resolve, reject) => {
      this.actions.push({
        kind: 'action',
        key: '',
        start: async () => {
          try {
            resolve(await run());
          } catch (err) {
            reject(err);
          }
        },
        skip: () => undefined,
      });
      this.pump();
    });
  }

  // A read the page repeats: not run (at once) while the same poll waits or
  // runs, or while polls are held. `run` gets a signal the page's going aborts.
  poll<T>(key: string, run: (signal: AbortSignal) => Promise<T>): Promise<PollOutcome<T>> {
    if (this.pollsPaused || this.pollKeys.has(key)) return Promise.resolve({ ran: false });
    this.pollKeys.add(key);
    return new Promise<PollOutcome<T>>((resolve, reject) => {
      this.polls.push({
        kind: 'poll',
        key,
        start: async () => {
          const controller = new AbortController();
          this.pollInFlight = controller;
          try {
            const value = await run(controller.signal);
            // An answer that came after the page went is no longer the page's.
            resolve(this.disposed ? { ran: false } : { ran: true, value });
          } catch (err) {
            reject(err);
          } finally {
            if (this.pollInFlight === controller) this.pollInFlight = null;
          }
        },
        skip: () => resolve({ ran: false }),
      });
      this.pump();
    });
  }

  // The page has gone: its waiting polls are dropped and the one in flight
  // aborted (the service still finishes it). Waiting actions still run.
  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    for (const task of this.polls.splice(0)) {
      this.pollKeys.delete(task.key);
      task.skip();
    }
    this.pollInFlight?.abort();
  }

  // Starts the next call once none is in flight: an action first, then a poll
  // (one held by a hidden tab or a gone page is dropped instead).
  private pump(): void {
    while (!this.running) {
      const task = this.actions.shift() ?? this.polls.shift();
      if (!task) return;
      if (task.kind === 'poll' && this.pollsPaused) {
        this.pollKeys.delete(task.key);
        task.skip();
        continue;
      }
      this.running = true;
      void task.start().finally(() => {
        this.running = false;
        if (task.kind === 'poll') this.pollKeys.delete(task.key);
        this.pump();
      });
    }
  }
}
