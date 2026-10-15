// One hosted server's console (components/HostedServerConsole.vue): its
// players, its rotation, the missions it has, its chat window and its console
// log, and the commands that change the server. The reads the service admits
// (one console request per account at once) go through the page's console
// queue (useConsoleQueue) under this console's own scope, so two consoles on
// one page (the admin's) never drop each other's; the log's read, which the
// service does not admit (GET .../console reads its database alone), runs
// beside the queue, one at a time. The reads repeat every few seconds while
// the tab is shown: the players, the chat and the log every tick, the rotation
// every third, the mission catalog until it is read once. A read still waiting
// from the last tick is not asked again. A command takes the queue's next turn,
// ahead of the waiting reads, and the panels it changed are read again after
// it. When the console goes, its waiting reads leave the queue. A server that
// is not running gets the log alone (logOnly), read once when it has ended
// (follow false).

import { onMounted, onUnmounted, ref, shallowRef } from 'vue';
import {
  CONSOLE_KEEP,
  addRotationMission,
  consolePlayerAction,
  cycleRotation,
  fetchConsoleChat,
  fetchConsoleLog,
  fetchConsoleMissions,
  fetchConsolePlayers,
  fetchConsoleRotation,
  isConsoleGone,
  rotationEntryAction,
  sendConsoleChat,
  type ConsoleError,
  type ConsoleErrorCode,
  type ConsoleResult,
  type ConsoleTarget,
} from '../api/hostedServer';
import type {
  ConsoleLine,
  ConsoleLink,
  ConsoleMission,
  ConsolePlayer,
  PlayerAction,
  RotationEntry,
  RotationEntryAction,
} from '../types/hostedServer';
import type { ConsoleQueue } from '../utils/consoleQueue';

// The reads' cadence: a tick every 5 s (the players, the chat, the log), the
// rotation every third tick. Well inside the owner's read bucket (two a
// second, README "Hosted servers").
export const CONSOLE_TICK_MS = 5_000;
const ROTATION_EVERY_TICKS = 3;

// The refusals a read meets in the normal run of things and is asked again
// for: a map load stalls the server's console for a few seconds (504
// admin_timeout), the link's queue or the service's console fills under load
// (503 admin_busy, console_full), and another tab of the account has its turn
// (429 console_busy). A read shows one only once it has met them this many
// times in a row.
const TRANSIENT_READ_CODES: ReadonlySet<ConsoleErrorCode> = new Set<ConsoleErrorCode>([
  'admin_timeout',
  'admin_busy',
  'console_full',
  'console_busy',
]);
const TRANSIENT_READS_SHOWN_AFTER = 3;

// The panels a command reports on.
export type ConsolePanel = 'players' | 'rotation' | 'chat';

// What a command came to, shown in its panel: done, a quiet "between
// matches", or a refusal.
export interface ConsoleNotice {
  tone: 'ok' | 'quiet' | 'error';
  text: string;
}

export interface HostedConsoleOptions {
  // Read the mission catalog (the owner's add has a picker; the admin's view
  // has no add).
  missions: boolean;
  // A refusal said the server is not running any more: the page reads it again.
  onGone: () => void;
  // The credentials were refused.
  onUnauthorized: () => void;
  // Read the console log alone (a server that is not running: no command
  // reaches it, but its log reads, an ended server's too).
  logOnly?: boolean;
  // Keep reading on the tick (the default); false reads once (an ended
  // server's log no longer changes).
  follow?: boolean;
}

type ReadKey = 'players' | 'rotation' | 'missions' | 'chat' | 'log';

// The console's scope in the page's queue: one per server.
function scopeOf(target: ConsoleTarget): string {
  return `${target.kind}:${target.id}`;
}

export function useHostedConsole(target: ConsoleTarget, queue: ConsoleQueue, options: HostedConsoleOptions) {
  const scope = scopeOf(target);
  // Null until first read.
  const players = shallowRef<ConsolePlayer[] | null>(null);
  const rotation = shallowRef<RotationEntry[] | null>(null);
  const missions = shallowRef<ConsoleMission[] | null>(null);
  const chat = shallowRef<string[] | null>(null);
  // The server is between matches (409 not_in_game): the player list and the
  // rotation are empty there.
  const playersBetween = ref(false);
  const rotationBetween = ref(false);
  // The console ring as read so far, oldest first, at most CONSOLE_KEEP lines.
  const log = shallowRef<ConsoleLine[]>([]);
  const logRead = ref(false);
  const link = shallowRef<ConsoleLink | null>(null);
  // Each read's last failure, '' once it answers again.
  const troubles = ref<Record<ReadKey, string>>({ players: '', rotation: '', missions: '', chat: '', log: '' });
  // Why the console is shut, '' while it answers: the server is not running
  // (the page reads it again and unmounts this; until then the reads keep
  // trying, and the next answer opens it again), or the credentials were
  // refused (the reads stop).
  const ended = ref('');
  // The command in flight: the controls wait for it.
  const acting = ref(false);
  const notices = ref<Record<ConsolePanel, ConsoleNotice | null>>({ players: null, rotation: null, chat: null });
  // The last command's outcome for a screen reader.
  const announcement = ref('');
  const alertText = ref('');

  let lastLineId = 0;
  let tick = 0;
  let timer: ReturnType<typeof setInterval> | undefined;
  // A 429 rate_limited's Retry-After: no tick reads before it.
  let holdUntil = 0;
  // The reads have ended: the credentials were refused, or the console unmounted.
  let stopped = false;
  // Each read's transient refusals in a row.
  const transientRuns: Record<ReadKey, number> = { players: 0, rotation: 0, missions: 0, chat: 0, log: 0 };
  // The log's read in flight (it runs beside the queue): its abort.
  let logInFlight: AbortController | null = null;

  function stop() {
    stopped = true;
    if (timer) clearInterval(timer);
    timer = undefined;
  }

  // A refusal that shuts the console (true), or one to show and retry.
  function shutsConsole(error: ConsoleError): boolean {
    if (isConsoleGone(error.code)) {
      // The page is told once per shut.
      if (!ended.value) {
        ended.value = `${error.message} This page reads the server again.`;
        options.onGone();
      }
      return true;
    }
    if (error.code === 'unauthorized' || error.code === 'forbidden') {
      ended.value = error.message;
      stop();
      options.onUnauthorized();
      return true;
    }
    return false;
  }

  function readFailed(key: ReadKey, error: ConsoleError) {
    if (shutsConsole(error)) return;
    if (error.code === 'rate_limited') {
      holdUntil = Date.now() + (error.retryAfterSeconds ?? CONSOLE_TICK_MS / 1000) * 1000;
    }
    // A stall or a full queue: the next tick asks again, and it shows only
    // once it keeps happening.
    if (TRANSIENT_READ_CODES.has(error.code)) {
      transientRuns[key]++;
      if (transientRuns[key] < TRANSIENT_READS_SHOWN_AFTER) return;
    } else {
      transientRuns[key] = 0;
    }
    troubles.value = { ...troubles.value, [key]: error.message };
  }

  // A read answered: its trouble goes, and a console the server's state shut
  // is open again (the log's read answers in any state, so not it).
  function readOk(key: ReadKey) {
    transientRuns[key] = 0;
    if (troubles.value[key]) troubles.value = { ...troubles.value, [key]: '' };
    if (key !== 'log' && ended.value) ended.value = '';
  }

  // One read through the queue, in this console's scope: its answer, or null
  // when it did not run (the same read still waits, the tab is hidden, the
  // page or this console has gone) or the reads have stopped.
  async function read<T>(
    key: Exclude<ReadKey, 'log'>,
    fetch: (signal: AbortSignal) => Promise<ConsoleResult<T>>,
  ): Promise<ConsoleResult<T> | null> {
    if (stopped) return null;
    const outcome = await queue.poll(scope, key, fetch);
    if (!outcome.ran || stopped) return null;
    return outcome.value;
  }

  async function readPlayers() {
    const result = await read('players', (signal) => fetchConsolePlayers(target, signal));
    if (!result) return;
    if (result.ok || result.error.code === 'not_in_game') {
      readOk('players');
      playersBetween.value = !result.ok;
      players.value = result.ok ? result.value : [];
      return;
    }
    readFailed('players', result.error);
  }

  async function readRotation() {
    const result = await read('rotation', (signal) => fetchConsoleRotation(target, signal));
    if (!result) return;
    if (result.ok || result.error.code === 'not_in_game') {
      readOk('rotation');
      rotationBetween.value = !result.ok;
      rotation.value = result.ok ? result.value : [];
      return;
    }
    readFailed('rotation', result.error);
  }

  async function readMissions() {
    if (!options.missions || missions.value !== null) return;
    const result = await read('missions', (signal) => fetchConsoleMissions(target, signal));
    if (!result) return;
    if (result.ok) {
      readOk('missions');
      missions.value = result.value;
      return;
    }
    readFailed('missions', result.error);
  }

  async function readChat() {
    const result = await read('chat', (signal) => fetchConsoleChat(target, signal));
    if (!result) return;
    if (result.ok) {
      readOk('chat');
      chat.value = result.value;
      return;
    }
    readFailed('chat', result.error);
  }

  // The lines past the last one read (the whole ring the first time). Beside
  // the queue (the service admits no console turn for it), one at a time, and
  // like the queue's reads never while the tab is hidden or once the page has
  // gone.
  async function readLog() {
    if (stopped || logInFlight || queue.pollsPaused) return;
    const controller = new AbortController();
    logInFlight = controller;
    const result = await fetchConsoleLog(target, { after: lastLineId, limit: CONSOLE_KEEP }, controller.signal);
    if (logInFlight === controller) logInFlight = null;
    if (stopped || controller.signal.aborted) return;
    if (!result.ok) {
      readFailed('log', result.error);
      return;
    }
    readOk('log');
    link.value = result.value.link;
    logRead.value = true;
    const fresh = result.value.lines.filter((line) => line.id > lastLineId);
    if (fresh.length === 0) return;
    lastLineId = fresh[fresh.length - 1].id;
    const lines = log.value.concat(fresh);
    log.value = lines.length > CONSOLE_KEEP ? lines.slice(lines.length - CONSOLE_KEEP) : lines;
  }

  function onTick() {
    if (stopped || queue.pollsPaused || Date.now() < holdUntil) return;
    if (options.logOnly) {
      void readLog();
      return;
    }
    void readPlayers();
    void readChat();
    void readLog();
    if (tick % ROTATION_EVERY_TICKS === 0) void readRotation();
    void readMissions();
    tick++;
  }

  // Shown again: read at once rather than at the next tick.
  function onVisibility() {
    if (!document.hidden) onTick();
  }

  function setNotice(panel: ConsolePanel, notice: ConsoleNotice | null) {
    notices.value = { ...notices.value, [panel]: notice };
  }

  // One command through the queue's next turn. `done` words its success;
  // `reread` names the reads it changes (the log too, always: a command is
  // recorded there), read again after it answers, and after a refusal that
  // leaves the server's state unknown or changed under the page.
  async function command<T>(
    panel: ConsolePanel,
    run: () => Promise<ConsoleResult<T>>,
    done: (value: T) => string,
    reread: Array<() => Promise<void>>,
  ): Promise<boolean> {
    if (acting.value || ended.value) return false;
    acting.value = true;
    setNotice(panel, null);
    announcement.value = '';
    alertText.value = '';
    let result: ConsoleResult<T>;
    try {
      result = await queue.action(run);
    } finally {
      acting.value = false;
    }
    const readAgain = () => {
      for (const again of reread) void again();
      void readLog();
    };
    if (result.ok) {
      const text = done(result.value);
      setNotice(panel, { tone: 'ok', text });
      announcement.value = text;
      readAgain();
      return true;
    }
    const error = result.error;
    if (shutsConsole(error)) return false;
    if (error.code === 'not_in_game') {
      setNotice(panel, { tone: 'quiet', text: error.message });
      announcement.value = error.message;
      void readPlayers();
      void readRotation();
      return false;
    }
    setNotice(panel, { tone: 'error', text: error.message });
    alertText.value = error.message;
    // What a refusal says changed under the page, or left unknown (a command
    // that timed out may have gone out; another edit of the rotation was
    // running): read it again.
    if (error.code === 'admin_timeout' || error.code === 'rotation_busy') readAgain();
    if (error.code === 'player_not_found') void readPlayers();
    if (error.code === 'no_such_entry' || error.code === 'rotation_full' || error.code === 'last_mission') {
      void readRotation();
    }
    return false;
  }

  function playerName(player: ConsolePlayer): string {
    return player.name.trim() || `slot ${player.slot}`;
  }

  function playerAction(player: ConsolePlayer, action: PlayerAction): Promise<boolean> {
    const who = playerName(player);
    return command(
      'players',
      () => consolePlayerAction(target, player.slot, action),
      (reply) => {
        switch (action) {
          case 'punt':
            return `Kicked ${who}.`;
          case 'ban':
            return reply.listed
              ? `Banned ${who}: their PCID is on the server's ban list, and they were kicked.`
              : `Banned ${who}.`;
          case 'swap':
            return `Moved ${who} to the other team.`;
          case 'kill':
            return `Killed ${who}.`;
        }
      },
      [readPlayers],
    );
  }

  function addMission(mission: string, swapSides: boolean): Promise<boolean> {
    return command(
      'rotation',
      () => addRotationMission(target, mission, swapSides),
      () => `Added ${mission} to the end of the rotation.`,
      [readRotation],
    );
  }

  function entryAction(entry: RotationEntry, action: RotationEntryAction): Promise<boolean> {
    return command(
      'rotation',
      () => rotationEntryAction(target, entry.index, action),
      () =>
        action === 'remove'
          ? `Removed ${entry.file} from the rotation.`
          : `${entry.file} is the next map.`,
      [readRotation],
    );
  }

  function cycle(): Promise<boolean> {
    return command(
      'rotation',
      () => cycleRotation(target),
      () => 'The round is ending: the next map loads now.',
      [readRotation, readPlayers],
    );
  }

  function sendChat(text: string): Promise<boolean> {
    return command('chat', () => sendConsoleChat(target, text), () => `Sent “${text}”.`, [readChat]);
  }

  onMounted(() => {
    onTick();
    if (options.follow === false) return;
    timer = setInterval(onTick, CONSOLE_TICK_MS);
    document.addEventListener('visibilitychange', onVisibility);
  });

  // The console has gone: its reads leave the page's queue (the other
  // consoles' stay), and its log read is dropped.
  onUnmounted(() => {
    stop();
    document.removeEventListener('visibilitychange', onVisibility);
    queue.cancel(scope);
    logInFlight?.abort();
    logInFlight = null;
  });

  return {
    players,
    playersBetween,
    rotation,
    rotationBetween,
    missions,
    chat,
    log,
    logRead,
    link,
    troubles,
    ended,
    acting,
    notices,
    announcement,
    alertText,
    playerName,
    playerAction,
    addMission,
    entryAction,
    cycle,
    sendChat,
  };
}
