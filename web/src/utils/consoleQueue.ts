// One page's hosted-console calls, one at a time. The service runs at most one
// console request per account at once and answers a second 429 console_busy
// at once, with no wait (apps/novaworld_common/hosted/README.md, "The admission"), so
// every console call a page makes that the service admits, its polls and its
// actions alike, goes through the page's one queue
// (composables/useConsoleQueue.ts), each starting only once the one before it
// has settled. (GET .../console reads the database alone and is not admitted,
// so the console log's read keeps out of it.)
// - an action (a kick, an add, a chat line, ...) runs before every poll still
//   waiting, right after the call in flight;
// - a poll is dropped while the same poll (its console's scope and its key)
//   still waits or runs, so a slow server never stacks them, while two
//   consoles on one page (the admin's) each keep their own;
// - while the tab is hidden, and for good once the page has gone (dispose),
//   no poll runs: a waiting one settles as not run, and at dispose the poll in
//   flight is aborted. A console that closes drops its own polls (cancel). An
//   action still runs: the person asked for it.

export type PollOutcome<T> = { ran: true; value: T } | { ran: false };

interface Task {
  kind: 'action' | 'poll';
  // A poll's console (its scope) and its scoped key; '' for an action.
  scope: string;
  key: string;
  // Runs the call and settles its caller's promise; never rejects.
  start: () => Promise<void>;
  // Settles a poll's caller as not run.
  skip: () => void;
}

export class ConsoleQueue {
  private readonly actions: Task[] = [];
  private readonly polls: Task[] = [];
  // The polls waiting or running, by scoped key.
  private readonly pollKeys = new Set<string>();
  private running = false;
  private disposed = false;
  // The poll in flight: its scope and its abort.
  private pollInFlight: { scope: string; controller: AbortController } | null = null;

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
        scope: '',
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

  // A read the page repeats, for the console `scope` names: not run (at once)
  // while the same poll of that console waits or runs, or while polls are
  // held. `run` gets a signal the page's going, or its console's, aborts.
  poll<T>(scope: string, key: string, run: (signal: AbortSignal) => Promise<T>): Promise<PollOutcome<T>> {
    const scoped = `${scope}:${key}`;
    if (this.pollsPaused || this.pollKeys.has(scoped)) return Promise.resolve({ ran: false });
    this.pollKeys.add(scoped);
    return new Promise<PollOutcome<T>>((resolve, reject) => {
      this.polls.push({
        kind: 'poll',
        scope,
        key: scoped,
        start: async () => {
          const inFlight = { scope, controller: new AbortController() };
          this.pollInFlight = inFlight;
          try {
            const value = await run(inFlight.controller.signal);
            // An answer that came after the page or its console went is no
            // longer theirs.
            resolve(this.disposed || inFlight.controller.signal.aborted ? { ran: false } : { ran: true, value });
          } catch (err) {
            reject(err);
          } finally {
            if (this.pollInFlight === inFlight) this.pollInFlight = null;
          }
        },
        skip: () => resolve({ ran: false }),
      });
      this.pump();
    });
  }

  // A console has gone: its waiting polls are dropped and its poll in flight
  // aborted (the service still finishes it), so none of them takes the
  // account's turn or holds its keys for a console opened again.
  cancel(scope: string): void {
    this.dropPolls((task) => task.scope === scope);
    if (this.pollInFlight?.scope === scope) this.pollInFlight.controller.abort();
  }

  // The page has gone: every waiting poll is dropped and the one in flight
  // aborted. Waiting actions still run.
  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    this.dropPolls(() => true);
    this.pollInFlight?.controller.abort();
  }

  private dropPolls(which: (task: Task) => boolean): void {
    for (let i = this.polls.length - 1; i >= 0; i--) {
      const task = this.polls[i];
      if (!which(task)) continue;
      this.polls.splice(i, 1);
      this.pollKeys.delete(task.key);
      task.skip();
    }
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
