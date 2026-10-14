<template>
  <section class="space-y-6" :aria-labelledby="ids.heading" data-seam="hosted-console">
    <div class="flex flex-col gap-2 sm:flex-row sm:items-baseline sm:justify-between">
      <component :is="headingTag" :id="ids.heading" class="text-lg font-semibold text-ink">Server console</component>
      <p class="text-xs text-ink-muted">
        Console link:
        <span :class="linkClass">{{ linkLabel }}</span>
        <template v-if="link?.state === 'up' && link.game_state">
          · {{ link.game_state === 'Game' ? 'in a match' : link.game_state === 'Menus' ? 'between matches' : 'scene unknown' }}
        </template>
      </p>
    </div>

    <!-- Live regions, always in the console so a screen reader hears what lands
         in them; each panel shows its visible copy. -->
    <p role="status" class="sr-only">{{ announcement }}</p>
    <p role="alert" class="sr-only">{{ alertText }}</p>

    <p v-if="ended" class="rounded-panel border border-border bg-surface p-3 text-sm text-ink-muted">{{ ended }}</p>
    <p v-else-if="trouble" class="text-xs text-warn">
      The console's last refresh did not get through: {{ trouble }} It keeps trying.
    </p>

    <!-- PLAYER LIST, with punt / ban / swap / kill for the owner. -->
    <section class="space-y-3" :aria-labelledby="ids.players">
      <div class="flex flex-wrap items-baseline justify-between gap-2">
        <component :is="subheadingTag" :id="ids.players" class="text-base font-semibold text-ink">Players</component>
        <span v-if="players && !playersBetween" class="text-xs text-ink-muted">
          {{ players.length }} playing
        </span>
      </div>
      <p v-if="notices.players" aria-hidden="true" :class="noticeClass(notices.players)">{{ notices.players.text }}</p>
      <p v-if="players === null" class="text-sm text-ink-muted">Reading the player list…</p>
      <p v-else-if="playersBetween" :class="betweenClass">
        Between matches: the player list comes back once the next map has loaded.
      </p>
      <p v-else-if="players.length === 0" class="text-sm text-ink-muted">No one is playing right now.</p>
      <!-- relative: the rows' sr-only spans are absolutely positioned, and without a
           positioned ancestor they escape this scroller and widen the page on a phone. -->
      <div v-else class="relative overflow-x-auto rounded-panel border border-border">
        <table class="min-w-full divide-y divide-border text-sm text-ink">
          <caption class="sr-only">Players on {{ serverName || 'the server' }}</caption>
          <thead>
            <tr class="bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted">
              <th scope="col" class="px-3 py-2">Slot</th>
              <th scope="col" class="px-3 py-2">Name</th>
              <th scope="col" class="px-3 py-2 text-right">Team</th>
              <th scope="col" class="px-3 py-2 text-right">Class</th>
              <th scope="col" class="px-3 py-2 text-right">Kills</th>
              <th scope="col" class="px-3 py-2 text-right">Deaths</th>
              <th scope="col" class="px-3 py-2 text-right">Ping</th>
              <th v-if="isOwner" scope="col" class="px-3 py-2 text-right">Actions</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border">
            <tr v-for="player in players" :key="player.slot">
              <td class="px-3 py-2 font-mono text-xs">{{ player.slot }}</td>
              <th scope="row" class="break-all px-3 py-2 text-left font-medium">{{ playerName(player) }}</th>
              <td class="px-3 py-2 text-right font-mono text-xs">{{ player.team }}</td>
              <td class="px-3 py-2 text-right font-mono text-xs">{{ player.class }}</td>
              <td class="px-3 py-2 text-right font-mono text-xs">{{ player.kills }}</td>
              <td class="px-3 py-2 text-right font-mono text-xs">{{ player.deaths }}</td>
              <td class="px-3 py-2 text-right font-mono text-xs">{{ player.ping }}</td>
              <td v-if="isOwner" class="space-x-3 whitespace-nowrap px-3 py-2 text-right">
                <button type="button" :disabled="controlsOff" :class="linkButton" @click="onPlayer(player, 'swap')">
                  Swap team<span class="sr-only">, {{ playerName(player) }}</span>
                </button>
                <button type="button" :disabled="controlsOff" :class="warnLinkButton" @click="onPlayer(player, 'kill')">
                  Kill<span class="sr-only">, {{ playerName(player) }}</span>
                </button>
                <button type="button" :disabled="controlsOff" :class="dangerLinkButton" @click="onPlayer(player, 'punt')">
                  Kick<span class="sr-only">, {{ playerName(player) }}</span>
                </button>
                <button type="button" :disabled="controlsOff" :class="dangerLinkButton" @click="onPlayer(player, 'ban')">
                  Ban<span class="sr-only">, {{ playerName(player) }}</span>
                </button>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </section>

    <!-- MISSION LIST, with remove / set next per entry, MISSION ADD from MISSION
         AVAILABLE (the owner's), and MISSION CYCLE. -->
    <section class="space-y-3" :aria-labelledby="ids.rotation">
      <div class="flex flex-wrap items-baseline justify-between gap-2">
        <component :is="subheadingTag" :id="ids.rotation" class="text-base font-semibold text-ink">Rotation</component>
        <span v-if="rotation && !rotationBetween" class="text-xs" :class="rotationFull ? 'text-warn' : 'text-ink-muted'">
          {{ rotation.length }} of {{ MISSIONS_MAX }} maps
        </span>
      </div>
      <p :id="ids.rotationHint" class="text-xs text-ink-muted">
        The server plays these maps in order and starts over.
        <template v-if="isOwner">
          Play next picks the map that follows this one; the last map always stays.
        </template>
        Cycle ends the round now and loads the next map.
      </p>
      <p v-if="notices.rotation" aria-hidden="true" :class="noticeClass(notices.rotation)">{{ notices.rotation.text }}</p>
      <p v-if="rotation === null" class="text-sm text-ink-muted">Reading the rotation…</p>
      <p v-else-if="rotationBetween" :class="betweenClass">
        Between matches: the rotation comes back once the next map has loaded.
      </p>
      <p v-else-if="rotation.length === 0" class="text-sm text-ink-muted">The rotation is empty.</p>
      <ol
        v-else
        :aria-labelledby="ids.rotation"
        class="divide-y divide-border rounded-panel border border-border"
      >
        <li
          v-for="entry in rotation"
          :key="entry.index"
          class="flex flex-wrap items-center gap-2 px-3 py-2 text-sm"
          :aria-current="entry.current ? 'true' : undefined"
        >
          <span class="w-6 shrink-0 text-right font-mono text-xs text-ink-muted">{{ entry.index }}.</span>
          <span class="min-w-0 flex-1 break-all font-mono text-ink">{{ entry.file }}</span>
          <span v-if="entry.current" class="rounded-control bg-online/15 px-2 py-0.5 text-xs text-online">
            Playing now<template v-if="entry.flipped">, second half</template>
          </span>
          <span v-if="entry.next" class="rounded-control bg-accent/15 px-2 py-0.5 text-xs text-accent">Next</span>
          <span v-if="entry.swap_sides" class="rounded-control bg-hover px-2 py-0.5 text-xs text-ink-muted">Sides swap at the half</span>
          <span v-if="entry.one_shot" class="rounded-control bg-hover px-2 py-0.5 text-xs text-ink-muted">Plays once</span>
          <span v-if="isOwner" class="flex shrink-0 gap-1">
            <button
              type="button"
              :disabled="controlsOff || entry.next"
              :class="smallButton"
              @click="onEntry(entry, 'next')"
            >
              Play next<span class="sr-only">: {{ entry.file }}</span>
            </button>
            <button
              type="button"
              :disabled="controlsOff || rotation.length <= 1"
              :class="smallDangerButton"
              @click="onEntry(entry, 'remove')"
            >
              Remove<span class="sr-only"> {{ entry.file }}</span>
            </button>
          </span>
        </li>
      </ol>

      <form
        v-if="isOwner && rotation !== null && !rotationBetween"
        class="space-y-2"
        :aria-describedby="ids.addHint"
        @submit.prevent="onAdd"
      >
        <label :for="ids.addMission" class="block text-xs font-medium uppercase tracking-wider text-ink-muted">
          Add a map to the end
        </label>
        <div class="flex flex-wrap items-center gap-2">
          <select
            :id="ids.addMission"
            v-model="addChoice"
            :disabled="addable.length === 0 || rotationFull"
            class="min-w-0 max-w-full flex-1 rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none disabled:opacity-50"
          >
            <option value="" disabled>{{ missions === null ? 'Reading the missions…' : 'Choose a mission' }}</option>
            <option v-for="mission in addable" :key="mission.file" :value="mission.file">
              {{ mission.title ? `${mission.title} (${mission.file})` : mission.file }}
            </option>
          </select>
          <label class="flex items-center gap-2 text-sm text-ink">
            <input
              v-model="addSwap"
              type="checkbox"
              class="h-4 w-4 rounded border-border bg-surface text-accent focus:ring-accent"
            />
            Swap sides at the half
          </label>
          <button type="submit" :disabled="controlsOff || !addChoice || rotationFull" :class="primaryButton">Add</button>
        </div>
        <p :id="ids.addHint" class="text-xs text-ink-muted">
          <template v-if="rotationFull">
            The rotation is full: it holds {{ MISSIONS_MAX }} maps at most. Remove one to add another.
          </template>
          <template v-else-if="missions !== null && addable.length === 0">
            The server has no mission to add.
          </template>
          <template v-else>
            From the missions the server has. The rotation holds {{ MISSIONS_MAX }} maps at most.
          </template>
        </p>
      </form>

      <button
        type="button"
        :disabled="controlsOff || rotationBetween"
        :class="warnButton"
        :aria-describedby="ids.rotationHint"
        @click="onCycle"
      >
        Cycle to the next map…
      </button>
    </section>

    <!-- CHAT GET, and the owner's CHAT SEND. -->
    <section class="space-y-3" :aria-labelledby="ids.chat">
      <component :is="subheadingTag" :id="ids.chat" class="text-base font-semibold text-ink">Chat</component>
      <p v-if="notices.chat" aria-hidden="true" :class="noticeClass(notices.chat)">{{ notices.chat.text }}</p>
      <p v-if="chat === null" class="text-sm text-ink-muted">Reading the chat…</p>
      <p v-else-if="chat.length === 0" class="text-sm text-ink-muted">Nothing said in the chat window yet.</p>
      <ol
        v-else
        ref="chatScroller"
        class="max-h-64 space-y-1 overflow-y-auto rounded-panel border border-border bg-surface p-3 font-mono text-xs text-ink"
        tabindex="0"
        :aria-labelledby="ids.chat"
        @scroll="chatSticky.onScroll"
      >
        <li v-for="(line, index) in chat" :key="index" class="break-words">{{ line }}</li>
      </ol>
      <form v-if="isOwner" class="space-y-2" @submit.prevent="onSendChat">
        <label :for="ids.chatInput" class="block text-xs font-medium uppercase tracking-wider text-ink-muted">
          Say to everyone on the server
        </label>
        <div class="flex gap-2">
          <input
            :id="ids.chatInput"
            v-model="chatText"
            type="text"
            required
            :maxlength="CHAT_TEXT_MAX"
            autocomplete="off"
            :aria-invalid="chatProblem ? 'true' : undefined"
            :aria-describedby="ids.chatHint"
            class="min-w-0 flex-1 rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none"
          />
          <button type="submit" :disabled="controlsOff" :class="primaryButton">Send</button>
        </div>
        <p :id="ids.chatHint" class="text-xs" :class="chatProblem ? 'text-danger' : 'text-ink-muted'">
          <template v-if="chatProblem">{{ chatProblem }}</template>
          <template v-else>
            {{ cp1252Length(chatText.trim()) }} of {{ CHAT_TEXT_MAX }} characters. The server drops a line repeated
            too quickly.
          </template>
        </p>
      </form>
    </section>

    <!-- The console ring: the commands sent (with their sender), the replies and
         the link's own lines. -->
    <section class="space-y-3" :aria-labelledby="ids.log">
      <div class="flex flex-wrap items-baseline justify-between gap-2">
        <component :is="subheadingTag" :id="ids.log" class="text-base font-semibold text-ink">Console log</component>
        <span class="text-xs text-ink-muted">The newest {{ CONSOLE_KEEP }} lines</span>
      </div>
      <p v-if="!logRead" class="text-sm text-ink-muted">Reading the console log…</p>
      <p v-else-if="log.length === 0" class="text-sm text-ink-muted">Nothing in the console log yet.</p>
      <ol
        v-else
        ref="logScroller"
        class="max-h-80 space-y-0.5 overflow-y-auto rounded-panel border border-border bg-surface p-3 font-mono text-xs"
        tabindex="0"
        :aria-labelledby="ids.log"
        @scroll="logSticky.onScroll"
      >
        <li v-for="line in log" :key="line.id" class="flex gap-2">
          <time class="shrink-0 text-ink-muted" :datetime="isoOf(line.at)" :title="localOf(line.at)">{{ clockOf(line.at) }}</time>
          <span v-if="line.kind === 'command'" class="min-w-0 break-words text-accent">
            &gt; {{ line.text }}<span v-if="line.actor" class="text-ink-muted"> ({{ line.actor }})</span>
          </span>
          <span v-else-if="line.kind === 'reply'" class="min-w-0 break-words text-ink">{{ line.text }}</span>
          <span v-else class="min-w-0 break-words italic text-ink-muted">{{ line.text }}</span>
        </li>
      </ol>
    </section>
  </section>
</template>

<script setup lang="ts">
// A running hosted server's console (ADR 0053 d5): its players, its rotation,
// its chat and its console log, read through the service's console routes
// (api/hostedServer.ts) by useHostedConsole, every call the service admits in
// the page's one console queue (useConsoleQueue: the service runs one console
// request per account at once). The owner's view (HostedServerStatus, on /me/servers) has
// every command the owner's routes offer: kick, ban, swap and kill a player,
// add, remove and play-next a map, cycle, and chat. The admin's view
// (HostedSection, on /admin/hosted) reads the same panels and cycles.
import { computed, nextTick, ref, watch, type Ref } from 'vue';
import {
  CHAT_TEXT_MAX,
  CONSOLE_KEEP,
  type ConsoleTarget,
} from '../api/hostedServer';
import { useConsoleQueue } from '../composables/useConsoleQueue';
import { useHostedConsole, type ConsoleNotice } from '../composables/useHostedConsole';
import type { ConsolePlayer, PlayerAction, RotationEntry, RotationEntryAction } from '../types/hostedServer';
import { cp1252Length, cp1252Problem } from '../utils/cp1252';
import { MISSIONS_MAX, isMissionName } from '../utils/hostedSettings';
import { isoOf, localOf } from '../utils/utcTime';

const props = withDefaults(
  defineProps<{
    target: ConsoleTarget;
    // 'owner': every command; 'admin': the reads and cycle.
    mode: 'owner' | 'admin';
    // For the confirmations and the captions.
    serverName: string;
    // The heading level the console sits under (its panels take the next).
    headingLevel?: 2 | 3 | 4;
  }>(),
  { headingLevel: 3 },
);

const emit = defineEmits<{
  // The server is not running any more: read it again.
  (e: 'gone'): void;
  // The credentials were refused.
  (e: 'unauthorized'): void;
}>();

const isOwner = computed(() => props.mode === 'owner');
const headingTag = computed(() => `h${props.headingLevel}`);
const subheadingTag = computed(() => `h${props.headingLevel + 1}`);

const ids = computed(() => {
  const base = props.target.kind === 'admin' ? `hosted-console-${props.target.id}` : 'hosted-console-own';
  return {
    heading: `${base}-heading`,
    players: `${base}-players`,
    rotation: `${base}-rotation`,
    rotationHint: `${base}-rotation-hint`,
    addMission: `${base}-add-mission`,
    addHint: `${base}-add-hint`,
    chat: `${base}-chat`,
    chatInput: `${base}-chat-input`,
    chatHint: `${base}-chat-hint`,
    log: `${base}-log`,
  };
});

const primaryButton =
  'rounded-control bg-accent px-4 py-2 text-sm font-medium text-on-accent transition hover:bg-accent/90 disabled:cursor-not-allowed disabled:opacity-50';
const warnButton =
  'rounded-control border border-warn/60 px-4 py-2 text-sm font-medium text-warn transition hover:bg-warn/10 disabled:cursor-not-allowed disabled:opacity-40';
const smallButton =
  'rounded-control border border-border px-2 py-1 text-xs text-ink transition hover:bg-hover disabled:opacity-40';
const smallDangerButton =
  'rounded-control border border-border px-2 py-1 text-xs text-danger/80 transition hover:text-danger disabled:opacity-40';
const linkButton = 'text-ink hover:text-ink-bright disabled:opacity-40';
const warnLinkButton = 'text-warn/90 hover:text-warn disabled:opacity-40';
const dangerLinkButton = 'text-danger/80 hover:text-danger disabled:opacity-40';
const betweenClass = 'rounded-panel border border-dashed border-border-strong p-3 text-sm text-ink-muted';

const {
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
} = useHostedConsole(props.target, useConsoleQueue(), {
  missions: props.mode === 'owner',
  onGone: () => emit('gone'),
  onUnauthorized: () => emit('unauthorized'),
});

const controlsOff = computed(() => acting.value || ended.value !== '');

// The first read still failing, if any.
const trouble = computed(() => Object.values(troubles.value).find((t) => t !== '') ?? '');

const linkLabel = computed(() => {
  switch (link.value?.state) {
    case 'up':
      return 'up';
    case 'connecting':
      return 'connecting';
    case 'down':
      return 'down, reconnecting';
    case 'stopped':
      return 'closed';
    default:
      return logRead.value ? 'none' : '…';
  }
});

const linkClass = computed(() => {
  switch (link.value?.state) {
    case 'up':
      return 'font-medium text-online';
    case 'connecting':
    case 'down':
      return 'font-medium text-warn';
    default:
      return 'font-medium text-ink-muted';
  }
});

function noticeClass(notice: ConsoleNotice): string {
  switch (notice.tone) {
    case 'ok':
      return 'rounded-panel border border-online/40 bg-online/10 p-3 text-sm text-online';
    case 'quiet':
      return 'rounded-panel border border-border bg-surface p-3 text-sm text-ink-muted';
    case 'error':
      return 'rounded-panel border border-danger/40 bg-danger/10 p-3 text-sm text-danger';
  }
}

// "14:03:27", the visitor's own clock.
function clockOf(utc: string): string {
  const t = Date.parse(isoOf(utc));
  return Number.isNaN(t) ? utc : new Date(t).toLocaleTimeString();
}

function serverLabel(): string {
  return props.serverName || 'the server';
}

// Ban and kill ask first; a kick and a swap go at once.
async function onPlayer(player: ConsolePlayer, action: PlayerAction) {
  const who = playerName(player);
  if (action === 'ban' && !window.confirm(`Ban ${who} from ${serverLabel()}? They are kicked, and their PCID goes on the server's ban list.`)) return;
  if (action === 'kill' && !window.confirm(`Kill ${who}?`)) return;
  await playerAction(player, action);
}

async function onEntry(entry: RotationEntry, action: RotationEntryAction) {
  await entryAction(entry, action);
}

async function onCycle() {
  if (!window.confirm(`Cycle ${serverLabel()} to the next map? The round ends now for everyone playing.`)) return;
  await cycle();
}

// The rotation picker: the missions the server has whose file an add takes.
const addChoice = ref('');
const addSwap = ref(false);
const addable = computed(() => (missions.value ?? []).filter((m) => isMissionName(m.file)));
const rotationFull = computed(() => (rotation.value?.length ?? 0) >= MISSIONS_MAX);

async function onAdd() {
  const mission = addChoice.value;
  if (!mission || rotationFull.value) return;
  if (await addMission(mission, addSwap.value)) {
    addChoice.value = '';
    addSwap.value = false;
  }
}

const chatText = ref('');
// The page's own check of the line (what the service would refuse), once
// something is typed.
const chatProblem = computed(() => {
  const text = chatText.value.trim();
  return text ? cp1252Problem(text, CHAT_TEXT_MAX) : null;
});

async function onSendChat() {
  const text = chatText.value.trim();
  if (!text || chatProblem.value) return;
  if (await sendChat(text)) chatText.value = '';
}

// A scroller that follows its newest line while it is scrolled to the end, and
// stays put while someone reads further up.
function stickToEnd(scroller: Ref<HTMLElement | null>, source: () => unknown) {
  let atEnd = true;
  const onScroll = () => {
    const el = scroller.value;
    if (el) atEnd = el.scrollHeight - el.scrollTop - el.clientHeight < 24;
  };
  watch(source, async () => {
    if (!atEnd) return;
    await nextTick();
    const el = scroller.value;
    if (el) el.scrollTop = el.scrollHeight;
  });
  return { onScroll };
}

const logScroller = ref<HTMLElement | null>(null);
const chatScroller = ref<HTMLElement | null>(null);
const logSticky = stickToEnd(logScroller, () => log.value);
const chatSticky = stickToEnd(chatScroller, () => chat.value);
</script>
