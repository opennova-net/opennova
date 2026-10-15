// The page's one hosted-console queue (utils/consoleQueue.ts). A page with a
// console (the server page, pages/servers/ServerDetailLayout.vue) makes it in
// its setup with provideConsoleQueue, and every console panel inside it takes
// that one with useConsoleQueue, so all of the page's admitted console calls
// run one at a time: the service answers an account's second concurrent
// console request 429 console_busy. Each console polls under its own scope,
// and drops its waiting polls when it closes (ConsoleQueue.cancel).

import { inject, onUnmounted, provide, type InjectionKey } from 'vue';
import { ConsoleQueue } from '../utils/consoleQueue';

const CONSOLE_QUEUE: InjectionKey<ConsoleQueue> = Symbol('hosted-console-queue');

// Makes the page's queue and hands it to the page's components; it is
// disposed when the page unmounts (its polls end there).
export function provideConsoleQueue(): ConsoleQueue {
  const queue = new ConsoleQueue();
  provide(CONSOLE_QUEUE, queue);
  onUnmounted(() => queue.dispose());
  return queue;
}

// The queue of the page a console panel is on.
export function useConsoleQueue(): ConsoleQueue {
  const queue = inject(CONSOLE_QUEUE, null);
  if (!queue) {
    throw new Error('A hosted console needs its page to provide the console queue (provideConsoleQueue).');
  }
  return queue;
}
