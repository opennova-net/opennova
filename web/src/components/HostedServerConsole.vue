<template>
  <section class="space-y-6" :aria-labelledby="ids.heading" data-seam="hosted-console">
    <div class="flex flex-wrap items-center gap-3">
      <component :is="headingTag" :id="ids.heading" class="section-heading">Console</component>
      <Badge v-if="!logOnly" class="console-link" :tone="linkTone" :title="link?.last_poll_at ? `Last read ${link.last_poll_at} UTC` : undefined">{{ linkLabel }}</Badge>
    </div>

    <!-- Live regions, always in the console so a screen reader hears what lands
         in them; each panel shows its visible copy. -->
    <p role="status" class="sr-only">{{ announcement }}</p>
    <p role="alert" class="sr-only">{{ alertText }}</p>

    <p v-if="ended" class="text-sm text-ink-muted">{{ ended }}</p>
    <p v-else-if="trouble" class="text-xs text-warn">Last refresh failed: {{ trouble }}</p>

    <template v-if="!logOnly">
    <ServerAdminPanel :target="target" />
    <!-- PLAYER LIST, with punt / ban / swap / kill for the owner. -->
    <section class="space-y-2" :aria-labelledby="ids.players">
      <div class="flex flex-wrap items-baseline gap-2">
        <component :is="subheadingTag" :id="ids.players" class="font-semibold text-ink-bright">Players</component>
        <span v-if="players && !playersBetween" class="text-xs text-ink-muted">{{ players.length }}</span>
      </div>
      <p v-if="notices.players" aria-hidden="true" :class="noticeClass(notices.players)">{{ notices.players.text }}</p>
      <p v-if="players === null" class="text-sm text-ink-muted">Reading…</p>
      <p v-else-if="playersBetween" class="text-sm text-ink-muted">Between matches.</p>
      <p v-else-if="players.length === 0" class="text-sm text-ink-muted">No one is playing.</p>
      <div v-else class="table-wrap">
        <table class="data-table">
          <caption class="sr-only">Players on {{ serverName || 'the server' }}</caption>
          <thead>
            <tr>
              <th scope="col">Slot</th>
              <th scope="col">Name</th>
              <th scope="col" class="text-right">Team</th>
              <th scope="col" class="text-right">Class</th>
              <th scope="col" class="text-right">Kills</th>
              <th scope="col" class="text-right">Deaths</th>
              <th scope="col" class="text-right">Ping</th>
              <th v-if="isOwner" scope="col"><span class="sr-only">Actions</span></th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="player in players" :key="player.slot">
              <td class="font-mono text-xs">{{ player.slot }}</td>
              <th scope="row" class="break-all">
                {{ playerName(player) }}
                <Badge v-if="fromNovaLogic(player)" tone="info" class="player-novalogic ml-1" title="Came through NovaLogic's NovaWorld"
                  >NovaLogic<span class="sr-only">: came through NovaLogic's NovaWorld</span></Badge
                >
              </th>
              <td class="text-right font-mono text-xs">{{ player.team }}</td>
              <td class="text-right font-mono text-xs">{{ player.class }}</td>
              <td class="text-right font-mono text-xs">{{ player.kills }}</td>
              <td class="text-right font-mono text-xs">{{ player.deaths }}</td>
              <td class="text-right font-mono text-xs">{{ player.ping }}</td>
              <td v-if="isOwner" class="whitespace-nowrap text-right">
                <button type="button" :disabled="controlsOff" class="btn-ghost btn-sm" @click="onPlayer(player, 'swap')">
                  Swap team<span class="sr-only">, {{ playerName(player) }}</span>
                </button>
                <button type="button" :disabled="controlsOff" class="btn-ghost btn-sm" @click="onPlayer(player, 'kill')">
                  Kill<span class="sr-only">, {{ playerName(player) }}</span>
                </button>
                <button type="button" :disabled="controlsOff" class="btn-ghost btn-sm" @click="onPlayer(player, 'punt')">
                  Kick<span class="sr-only">, {{ playerName(player) }}</span>
                </button>
                <button type="button" :disabled="controlsOff" class="btn-ghost btn-sm" @click="onPlayer(player, 'ban')">
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
    <section class="space-y-2" :aria-labelledby="ids.rotation">
      <div class="flex flex-wrap items-center gap-2">
        <component :is="subheadingTag" :id="ids.rotation" class="font-semibold text-ink-bright">Rotation</component>
        <span v-if="rotation && !rotationBetween" class="text-xs" :class="rotationFull ? 'text-warn' : 'text-ink-muted'">{{ rotation.length }}/{{ MISSIONS_MAX }}</span>
        <button
          type="button"
          class="btn-warn btn-sm ml-auto"
          :disabled="controlsOff || rotationBetween"
          title="Ends the round and loads the next map"
          @click="onCycle"
        >
          Cycle to next map…
        </button>
      </div>
      <p v-if="notices.rotation" aria-hidden="true" :class="noticeClass(notices.rotation)">{{ notices.rotation.text }}</p>
      <p v-if="rotation === null" class="text-sm text-ink-muted">Reading…</p>
      <p v-else-if="rotationBetween" class="text-sm text-ink-muted">Between matches.</p>
      <p v-else-if="rotation.length === 0" class="text-sm text-ink-muted">Empty.</p>
      <!-- Capped, so a long rotation scrolls in place; play order puts the map playing now first. -->
      <ol v-else :aria-labelledby="ids.rotation" class="card max-h-96 divide-y divide-border overflow-y-auto" tabindex="0">
        <li
          v-for="(entry, position) in playOrder"
          :key="entry.index"
          class="flex flex-wrap items-center gap-2 px-3 py-1.5 text-sm"
          :aria-current="entry.current ? 'true' : undefined"
        >
          <span class="w-6 shrink-0 text-right font-mono text-xs text-ink-muted">{{ position + 1 }}.</span>
          <span class="min-w-0 flex-1 break-all">
            <template v-if="titleOf(entry)">{{ titleOf(entry) }} <span class="font-mono text-xs text-ink-muted">{{ entry.file }}</span></template>
            <span v-else class="font-mono">{{ entry.file }}</span>
          </span>
          <Badge v-if="entry.current" tone="ok">Playing<template v-if="entry.flipped">, 2nd half</template></Badge>
          <Badge v-if="entry.next" tone="info">Next</Badge>
          <Badge v-if="entry.swap_sides">Swap at half</Badge>
          <Badge v-if="entry.one_shot">Once</Badge>
          <span v-if="isOwner" class="flex shrink-0 gap-1">
            <button type="button" :disabled="controlsOff || entry.next" class="btn-ghost btn-sm" @click="onEntry(entry, 'next')">
              Play next<span class="sr-only">: {{ entry.file }}</span>
            </button>
            <button type="button" :disabled="controlsOff || rotation.length <= 1" class="btn-ghost btn-sm" @click="onEntry(entry, 'remove')">
              Remove<span class="sr-only">{{ ' ' + (entry.file) }}</span>
            </button>
          </span>
        </li>
      </ol>

      <form
        v-if="isOwner && rotation !== null && !rotationBetween"
        class="flex flex-wrap items-center gap-2"
        :aria-describedby="ids.addHint"
        @submit.prevent="onAdd"
      >
        <label :for="ids.addMission" class="sr-only">Add a map</label>
        <select :id="ids.addMission" v-model="addChoice" :disabled="addable.length === 0 || rotationFull" class="field w-auto min-w-0 max-w-full flex-1">
          <option value="" disabled>{{ missions === null ? 'Reading…' : 'Add a map' }}</option>
          <option v-for="mission in addable" :key="mission.file" :value="mission.file">
            {{ mission.title ? `${mission.title} (${mission.file})` : mission.file }}
          </option>
        </select>
        <label class="flex items-center gap-2 text-sm">
          <input v-model="addSwap" type="checkbox" class="h-4 w-4 accent-accent" />
          Swap at half
        </label>
        <label class="text-sm">Insert
          <select v-model="addAt" class="field w-auto"><option :value="null">At end</option><option v-for="entry in rotation ?? []" :key="entry.index" :value="entry.index">Before {{ entry.index + 1 }}: {{ entry.file }}</option></select>
        </label>
        <label class="flex items-center gap-2 text-sm"><input v-model="addOnce" type="checkbox" /> One play only</label>
        <button type="submit" :disabled="controlsOff || !addChoice || rotationFull" class="btn-primary btn-sm">Add</button>
        <p :id="ids.addHint" class="w-full text-xs text-ink-muted">
          <template v-if="rotationFull">Full: remove a map to add another.</template>
          <template v-else-if="missions !== null && addable.length === 0">No mission to add.</template>
        </p>
      </form>
    </section>

    <!-- CHAT GET, and the owner's CHAT SEND. -->
    <section class="space-y-2" :aria-labelledby="ids.chat">
      <component :is="subheadingTag" :id="ids.chat" class="font-semibold text-ink-bright">Chat</component>
      <p v-if="notices.chat" aria-hidden="true" :class="noticeClass(notices.chat)">{{ notices.chat.text }}</p>
      <p v-if="chat === null" class="text-sm text-ink-muted">Reading…</p>
      <p v-else-if="chat.length === 0" class="text-sm text-ink-muted">Nothing said yet.</p>
      <ol
        v-else
        ref="chatScroller"
        class="max-h-64 space-y-1 overflow-y-auto rounded-panel border border-border bg-surface p-3 font-mono text-xs text-ink"
        tabindex="0"
        :aria-labelledby="ids.chat"
        @scroll="chatSticky.onScroll"
      >
        <li v-for="(line, index) in chat" :key="index" class="whitespace-pre-wrap break-words">{{ line }}</li>
      </ol>
      <form v-if="isOwner" class="space-y-1" @submit.prevent="onSendChat">
        <label :for="ids.chatInput" class="sr-only">Say to everyone on the server</label>
        <div class="flex gap-2">
          <input
            :id="ids.chatInput"
            v-model="chatText"
            type="text"
            required
            :maxlength="CHAT_TEXT_MAX"
            autocomplete="off"
            placeholder="Say to everyone"
            :aria-invalid="chatProblem ? 'true' : undefined"
            :aria-describedby="ids.chatHint"
            class="field min-w-0 flex-1"
          />
          <button type="submit" :disabled="controlsOff" class="btn-primary">Send</button>
        </div>
        <p :id="ids.chatHint" class="text-xs" :class="chatProblem ? 'text-danger' : 'text-ink-muted'">
          {{ chatProblem || `${cp1252Length(chatText.trim())}/${CHAT_TEXT_MAX}` }}
        </p>
      </form>
    </section>
    </template>

    <!-- The console ring: the commands sent (with their sender), the replies and
         the link's own lines. -->
    <section class="space-y-2" :aria-labelledby="ids.log">
      <component :is="subheadingTag" :id="ids.log" class="font-semibold text-ink-bright" :title="`The newest ${CONSOLE_KEEP} lines`">Console log</component>
      <p v-if="!logRead" class="text-sm text-ink-muted">Reading…</p>
      <p v-else-if="log.length === 0" class="text-sm text-ink-muted">Empty.</p>
      <ol
        v-else
        ref="logScroller"
        class="max-h-80 space-y-0.5 overflow-y-auto rounded-panel border border-border bg-surface p-3 font-mono text-xs"
        tabindex="0"
        :aria-labelledby="ids.log"
        @scroll="logSticky.onScroll"
      >
        <li v-for="line in log" :key="line.id" class="flex gap-2">
          <time class="shrink-0 text-ink-muted" :datetime="isoTime(line.at)" :title="formatUtc(line.at)">{{ clockOf(line.at) }}</time>
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
// request per account at once). The owner's view (the server page's Console
// tab, /me/servers/<id>/console) has every command the owner's routes offer:
// kick, ban, swap and kill a player, add, remove and play-next a map, cycle,
// and chat. The admin's view reads the same panels and cycles. A server that
// is not running shows its console log alone (`logOnly`), read once when it
// has ended (`follow` false). Every id carries the target's kind and id.
import { computed, nextTick, ref, watch, type Ref } from 'vue';
import {
  CHAT_TEXT_MAX,
  CONSOLE_KEEP,
  type ConsoleTarget,
} from '../api/hostedServer';
import { confirmAction } from '../composables/useConfirm';
import { useConsoleQueue } from '../composables/useConsoleQueue';
import { useHostedConsole, type ConsoleNotice } from '../composables/useHostedConsole';
import type { ConsolePlayer, PlayerAction, RotationEntry, RotationEntryAction } from '../types/hostedServer';
import Badge from './ui/Badge.vue';
import ServerAdminPanel from './ServerAdminPanel.vue';
import { cp1252Length, cp1252Problem } from '../utils/cp1252';
import { MISSIONS_MAX, isMissionName } from '../utils/hostedSettings';
import { formatUtc, isoTime, timeMs } from '../utils/format';
import type { Tone } from '../utils/tone';

const props = withDefaults(
  defineProps<{
    target: ConsoleTarget;
    // 'owner': every command; 'admin': the reads and cycle.
    mode: 'owner' | 'admin';
    // For the confirmations and the captions.
    serverName: string;
    // The player slots (PLAYER LIST's) of the players who came through
    // NovaLogic's NovaWorld (the server's `novalogic.player_slots`), each
    // marked in the list: as of the server's last report, a few seconds behind.
    novalogicSlots?: readonly number[];
    // The heading level the console sits under (its panels take the next).
    headingLevel?: 2 | 3 | 4;
    // The console log alone (the server is not running).
    logOnly?: boolean;
    // Keep reading (the default); false reads once (an ended server).
    follow?: boolean;
  }>(),
  { headingLevel: 3, logOnly: false, follow: true, novalogicSlots: () => [] },
);

function fromNovaLogic(player: ConsolePlayer): boolean {
  return props.novalogicSlots.includes(player.slot);
}

const emit = defineEmits<{
  // The server is not running any more: read it again.
  (e: 'gone'): void;
  // The credentials were refused.
  (e: 'unauthorized'): void;
}>();

const isOwner = computed(() => props.mode === 'owner' || props.mode === 'admin');
const headingTag = computed(() => `h${props.headingLevel}`);
const subheadingTag = computed(() => `h${props.headingLevel + 1}`);

const ids = computed(() => {
  const base = `hosted-console-${props.target.kind}-${props.target.id}`;
  return {
    heading: `${base}-heading`,
    players: `${base}-players`,
    rotation: `${base}-rotation`,
    addMission: `${base}-add-mission`,
    addHint: `${base}-add-hint`,
    chat: `${base}-chat`,
    chatInput: `${base}-chat-input`,
    chatHint: `${base}-chat-hint`,
    log: `${base}-log`,
  };
});

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
  missions: props.mode === 'owner' && !props.logOnly,
  onGone: () => emit('gone'),
  onUnauthorized: () => emit('unauthorized'),
  logOnly: props.logOnly,
  follow: props.follow,
});

const controlsOff = computed(() => acting.value || ended.value !== '');

// The first read still failing, if any.
const trouble = computed(() => Object.values(troubles.value).find((t) => t !== '') ?? '');

// The service's link to the server, in a few words, and the scene it last read.
const linkLabel = computed(() => {
  const scene = link.value?.game_state === 'Game' ? ', in a match' : link.value?.game_state === 'Menus' ? ', between matches' : '';
  switch (link.value?.state) {
    case 'up':
      return `Link up${scene}`;
    case 'connecting':
      return 'Link connecting';
    case 'down':
      return 'Link down, reconnecting';
    case 'stopped':
      return 'Link closed';
    default:
      return logRead.value ? 'No link' : 'Link…';
  }
});

const linkTone = computed<Tone>(() => {
  switch (link.value?.state) {
    case 'up':
      return 'ok';
    case 'connecting':
    case 'down':
      return 'warn';
    default:
      return 'muted';
  }
});

function noticeClass(notice: ConsoleNotice): string {
  switch (notice.tone) {
    case 'ok':
      return 'notice notice-ok';
    case 'quiet':
      return 'notice text-ink-muted';
    case 'error':
      return 'notice notice-bad';
  }
}

// "14:03:27", the visitor's own clock.
function clockOf(utc: string): string {
  const t = timeMs(utc);
  return Number.isNaN(t) ? utc : new Date(t).toLocaleTimeString();
}

function serverLabel(): string {
  return props.serverName || 'the server';
}

// Ban and kill ask first; a kick and a swap go at once.
async function onPlayer(player: ConsolePlayer, action: PlayerAction) {
  const who = playerName(player);
  const expectedName = player.name;
  if (action === 'ban' && !await confirmAction(`Ban ${who} from ${serverLabel()}? They are kicked and their PCID banned.`)) return;
  if (action === 'kill' && !await confirmAction(`Kill ${who}?`)) return;
  // Polls keep running while the confirmation is open. Never send the old
  // slot's command after its player has changed in the latest roster.
  const current = players.value?.find((entry) => entry.slot === player.slot);
  if (!current || current.name !== expectedName) {
    const text = 'The player list changed. Choose a player from the current list and try again.';
    notices.value = { ...notices.value, players: { tone: 'error', text } };
    alertText.value = text;
    return;
  }
  await playerAction(current, action);
}

async function onEntry(entry: RotationEntry, action: RotationEntryAction) {
  await entryAction(entry, action);
}

async function onCycle() {
  if (!await confirmAction(`Cycle ${serverLabel()} to the next map? The round ends now for everyone playing.`)) return;
  await cycle();
}

// The rotation in play order from the map playing now (the server lists it
// from the map after it, the current one last), each with the title the
// server's missions give it.
const playOrder = computed(() => {
  const list = rotation.value ?? [];
  const at = list.findIndex((entry) => entry.current);
  return at <= 0 ? list : [...list.slice(at), ...list.slice(0, at)];
});
const titles = computed(() => new Map((missions.value ?? []).map((m) => [m.file.toLowerCase(), m.title])));
function titleOf(entry: RotationEntry): string {
  return titles.value.get(entry.file.toLowerCase()) ?? '';
}

// The rotation picker: the missions the server has whose file an add takes.
const addChoice = ref('');
const addSwap = ref(false);
const addAt = ref<number | null>(null), addOnce = ref(false);
const addable = computed(() => (missions.value ?? []).filter((m) => isMissionName(m.file)));
const rotationFull = computed(() => (rotation.value?.length ?? 0) >= MISSIONS_MAX);

async function onAdd() {
  const mission = addChoice.value;
  if (!mission || rotationFull.value) return;
  if (await addMission(mission, addSwap.value, addAt.value, addOnce.value)) {
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
