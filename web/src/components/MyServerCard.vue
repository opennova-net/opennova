<template>
  <article
    class="space-y-6 rounded-panel border border-border bg-panel p-6"
    :aria-labelledby="ids.name"
  >
    <header class="flex flex-col gap-3 sm:flex-row sm:items-start sm:justify-between">
      <div class="min-w-0">
        <h2 :id="ids.name" class="truncate text-2xl font-semibold text-ink">
          {{ server.server_name || 'Unnamed server' }}
        </h2>
        <p class="mt-1 text-sm text-ink-muted">
          <span class="text-ink">{{ server.mission_name || 'No mission yet' }}</span>
          <span v-if="server.game_type"> · {{ server.game_type }}</span>
          · {{ server.players }}/{{ server.max_players }} players
          · <span class="font-mono text-xs">{{ server.host_ip }}:{{ server.host_port }}</span>
        </p>
        <p v-if="server.msg" class="mt-1 text-sm text-ink-muted">
          Message: <span class="text-ink">{{ server.msg }}</span>
        </p>
      </div>
      <dl class="grid shrink-0 grid-cols-[auto_auto] gap-x-3 text-xs text-ink-muted sm:text-right">
        <dt>Listed</dt>
        <dd><time :datetime="isoOf(server.created_at)" :title="localOf(server.created_at)">{{ ago(server.created_at) }}</time></dd>
        <dt>Updated</dt>
        <dd><time :datetime="isoOf(server.updated_at)" :title="localOf(server.updated_at)">{{ ago(server.updated_at) }}</time></dd>
      </dl>
    </header>

    <!-- Live regions, always in the card so a screen reader hears what lands
         in them; the boxes below are their visible copies. -->
    <p role="status" class="sr-only">{{ status }}</p>
    <p role="alert" class="sr-only">{{ alertText }}</p>
    <p v-if="status" aria-hidden="true" class="rounded-panel border border-online/40 bg-online/10 p-3 text-sm text-online">
      {{ status }}
    </p>
    <p
      v-if="alertText"
      :id="ids.error"
      aria-hidden="true"
      class="rounded-panel border border-danger/40 bg-danger/10 p-3 text-sm text-danger"
    >
      {{ alertText }}
    </p>

    <div class="grid gap-6 lg:grid-cols-2">
      <!-- TextChatServer -->
      <form class="space-y-2" @submit.prevent="sendChat">
        <label :for="ids.chat" class="block text-xs font-medium uppercase tracking-wider text-ink-muted">
          Say to everyone on the server
        </label>
        <div class="flex gap-2">
          <input
            :id="ids.chat"
            v-model="chat"
            type="text"
            required
            maxlength="400"
            autocomplete="off"
            :aria-invalid="failedForm === 'chat' ? 'true' : undefined"
            :aria-describedby="failedForm === 'chat' ? ids.error : undefined"
            class="min-w-0 flex-1 rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none"
          />
          <button type="submit" :disabled="busy" :class="primaryButton">Send</button>
        </div>
      </form>

      <!-- SetServerName -->
      <form class="space-y-2" @submit.prevent="sendName">
        <label :for="ids.serverName" class="block text-xs font-medium uppercase tracking-wider text-ink-muted">
          Server name
        </label>
        <div class="flex gap-2">
          <input
            :id="ids.serverName"
            v-model="name"
            type="text"
            required
            maxlength="31"
            autocomplete="off"
            :placeholder="server.server_name"
            :aria-invalid="failedForm === 'name' ? 'true' : undefined"
            :aria-describedby="failedForm === 'name' ? `${ids.nameHint} ${ids.error}` : ids.nameHint"
            class="min-w-0 flex-1 rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none"
          />
          <button type="submit" :disabled="busy" :class="primaryButton">Rename</button>
        </div>
        <p :id="ids.nameHint" class="text-xs text-ink-muted">
          31 characters at most. The server saves it in its game.cfg; the browser shows it after the server's next update.
        </p>
      </form>

      <!-- SetServerMsg -->
      <form class="space-y-2" @submit.prevent="sendMessage">
        <label :for="ids.serverMsg" class="block text-xs font-medium uppercase tracking-wider text-ink-muted">
          Server message
        </label>
        <div class="flex gap-2">
          <input
            :id="ids.serverMsg"
            v-model="message"
            type="text"
            required
            maxlength="127"
            autocomplete="off"
            :placeholder="server.msg"
            :aria-invalid="failedForm === 'msg' ? 'true' : undefined"
            :aria-describedby="failedForm === 'msg' ? `${ids.msgHint} ${ids.error}` : ids.msgHint"
            class="min-w-0 flex-1 rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none"
          />
          <button type="submit" :disabled="busy" :class="primaryButton">Set</button>
        </div>
        <p :id="ids.msgHint" class="text-xs text-ink-muted">127 characters at most, saved in the server's game.cfg.</p>
      </form>

      <!-- Cycle / EndMission / GameOver -->
      <fieldset class="space-y-2" :aria-describedby="ids.missionHint">
        <legend class="text-xs font-medium uppercase tracking-wider text-ink-muted">Mission</legend>
        <div class="flex flex-wrap items-center gap-2">
          <label :for="ids.winner" class="text-sm text-ink">Winner</label>
          <select
            :id="ids.winner"
            v-model="winner"
            class="rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none"
          >
            <option value="">No winner</option>
            <option v-for="team in winnerTeams" :key="team" :value="team">{{ team }}</option>
          </select>
          <button type="button" :disabled="busy" :class="warnButton" @click="endMission('Cycle')">Next map</button>
          <button type="button" :disabled="busy" :class="warnButton" @click="endMission('EndMission')">End mission</button>
          <button type="button" :disabled="busy" :class="warnButton" @click="endMission('GameOver')">Game over</button>
        </div>
        <p :id="ids.missionHint" class="text-xs text-ink-muted">
          Each ends the running mission for everyone on the server, with the winner you pick, and the
          server moves on to the next map in its rotation (a stock host runs the three alike).
        </p>
      </fieldset>

      <!-- Earthquake / Lightning / TimeOfDay -->
      <fieldset class="space-y-2">
        <legend class="text-xs font-medium uppercase tracking-wider text-ink-muted">Weather and time</legend>
        <form class="flex flex-wrap items-center gap-2" @submit.prevent="sendQuake">
          <label :for="ids.quake" class="text-sm text-ink">Earthquake for</label>
          <input
            :id="ids.quake"
            v-model.number="quakeSeconds"
            type="number"
            min="0"
            max="40"
            required
            class="w-20 rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none"
          />
          <span class="text-sm text-ink-muted">seconds</span>
          <button type="submit" :disabled="busy" :class="secondaryButton">Shake</button>
          <button type="button" :disabled="busy" :class="secondaryButton" @click="sendLightning">Lightning</button>
        </form>
        <form class="flex flex-wrap items-center gap-2" @submit.prevent="sendTimeOfDay">
          <label :for="ids.timeOfDay" class="text-sm text-ink">Time of day</label>
          <input
            :id="ids.timeOfDay"
            v-model="timeOfDay"
            type="text"
            inputmode="numeric"
            required
            pattern="([01][0-9]|2[0-3])[0-5][0-9]"
            maxlength="4"
            placeholder="HHMM"
            :aria-invalid="failedForm === 'tod' ? 'true' : undefined"
            :aria-describedby="ids.todHint"
            class="w-24 rounded-control border border-border bg-surface px-3 py-2 font-mono text-sm text-ink focus:border-accent focus:outline-none"
          />
          <button type="submit" :disabled="busy" :class="secondaryButton">Set time</button>
        </form>
        <p :id="ids.todHint" class="text-xs text-ink-muted">24-hour time as four digits, 1330 for half past one.</p>
      </fieldset>
    </div>

    <!-- Players: PuntPlayer / KillPlayer / ChangeTeam, aimed ByIndex at a roster slot -->
    <section class="space-y-3" :aria-labelledby="ids.players">
      <h3 :id="ids.players" class="text-lg font-semibold text-ink">Players</h3>
      <p v-if="server.roster.length === 0" class="text-sm text-ink-muted">
        No players in the game, as of the server's last update.
      </p>
      <div v-else class="overflow-x-auto rounded-panel border border-border">
        <table class="min-w-full divide-y divide-border text-sm text-ink">
          <caption class="sr-only">Players on {{ server.server_name }}, as of the server's last update</caption>
          <thead>
            <tr class="bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted">
              <th scope="col" class="px-4 py-2">Slot</th>
              <th scope="col" class="px-4 py-2">Name</th>
              <th scope="col" class="px-4 py-2">Team</th>
              <th scope="col" class="px-4 py-2 text-right">Actions</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border">
            <tr v-for="slot in server.roster" :key="slot.slot">
              <td class="px-4 py-2 font-mono text-xs">{{ slot.slot }}</td>
              <th scope="row" class="px-4 py-2 text-left font-medium">{{ slot.player_name || '(unnamed)' }}</th>
              <td class="px-4 py-2">{{ teamLabel(slot.team) }}</td>
              <td class="space-x-3 whitespace-nowrap px-4 py-2 text-right">
                <button type="button" :disabled="busy" class="text-ink hover:text-ink-bright disabled:opacity-40" @click="playerAction('ChangeTeam', slot)">
                  Switch team<span class="sr-only">, {{ slot.player_name }}</span>
                </button>
                <button type="button" :disabled="busy" class="text-warn/90 hover:text-warn disabled:opacity-40" @click="playerAction('KillPlayer', slot)">
                  Kill<span class="sr-only">, {{ slot.player_name }}</span>
                </button>
                <button type="button" :disabled="busy" class="text-danger/80 hover:text-danger disabled:opacity-40" @click="playerAction('PuntPlayer', slot)">
                  Kick<span class="sr-only">, {{ slot.player_name }}</span>
                </button>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </section>

    <!-- ServerStopHosting (MsgCode 7) -->
    <section class="space-y-3 rounded-panel border border-danger/40 p-4" :aria-labelledby="ids.stop">
      <h3 :id="ids.stop" class="text-lg font-semibold text-danger">Stop hosting</h3>
      <p class="text-sm text-ink-muted">
        Ends the running match for everyone on the server: every player is dropped, the server
        goes back to its main menu and leaves NovaWorld.
      </p>
      <button
        v-if="!confirmingStop"
        ref="stopButton"
        type="button"
        :disabled="busy"
        class="rounded-control border border-danger px-4 py-2 text-sm font-medium text-danger transition hover:bg-danger/10 disabled:opacity-40"
        @click="beginStop"
      >
        Stop hosting…
      </button>
      <form v-else class="space-y-3" @submit.prevent="confirmStop">
        <p ref="stopWarning" tabindex="-1" class="text-sm font-semibold text-danger focus:outline-none">
          Stop hosting {{ server.server_name }}? The match ends for all {{ server.players }} player(s) at once. This cannot be undone from here.
        </p>
        <label class="flex items-center gap-3 text-sm text-ink">
          <input
            v-model="stopUnderstood"
            type="checkbox"
            class="h-4 w-4 rounded border-border bg-surface text-danger focus:ring-danger"
          />
          I understand this ends the match for everyone on the server.
        </label>
        <div class="flex flex-wrap gap-2">
          <button
            type="submit"
            :disabled="busy || !stopUnderstood"
            class="rounded-control bg-danger px-4 py-2 text-sm font-semibold text-on-accent transition hover:bg-danger/90 disabled:opacity-40"
          >
            Stop hosting now
          </button>
          <button type="button" :disabled="busy" :class="secondaryButton" @click="cancelStop">Cancel</button>
        </div>
      </form>
    </section>
  </article>
</template>

<script setup lang="ts">
import { computed, nextTick, ref } from 'vue';
import { sendServerCommand, stopHosting, type HostError } from '../api/myServers';
import type { MyServer, OwnerVerb, RosterSlot, ServerCommandBody } from '../types/myServers';

const props = defineProps<{
  server: MyServer;
  // The page's clock (ms), for the "ago" times.
  now: number;
}>();

const emit = defineEmits<{
  // A push may change the listing (or showed it gone): refresh the list.
  (e: 'changed'): void;
  // The stop went out; the server leaves the list.
  (e: 'stopped', name: string): void;
  // A push answered 401.
  (e: 'session-ended'): void;
}>();

const primaryButton =
  'rounded-control bg-accent px-4 py-2 text-sm font-medium text-on-accent transition hover:bg-accent/90 disabled:cursor-not-allowed disabled:opacity-50';
const secondaryButton =
  'rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40';
const warnButton =
  'rounded-control border border-warn/60 px-4 py-2 text-sm font-medium text-warn transition hover:bg-warn/10 disabled:cursor-not-allowed disabled:opacity-40';

// The ids the labels, hints and live regions point at, unique per server.
const ids = computed(() => {
  const base = `server-${props.server.rid}`;
  return {
    name: `${base}-name`,
    error: `${base}-error`,
    chat: `${base}-chat`,
    serverName: `${base}-server-name`,
    nameHint: `${base}-server-name-hint`,
    serverMsg: `${base}-server-msg`,
    msgHint: `${base}-server-msg-hint`,
    winner: `${base}-winner`,
    missionHint: `${base}-mission-hint`,
    quake: `${base}-quake`,
    timeOfDay: `${base}-time-of-day`,
    todHint: `${base}-time-of-day-hint`,
    players: `${base}-players`,
    stop: `${base}-stop`,
  };
});

// The winner tokens the Cycle / EndMission / GameOver reader takes (a team
// name or its number; none means no winner).
const winnerTeams = ['Blue', 'Red', 'Yellow', 'Violet'] as const;
const TEAM_NAMES: Record<string, string> = { '1': 'Blue', '2': 'Red', '3': 'Yellow', '4': 'Violet' };

const busy = ref(false);
const status = ref('');
const alertText = ref('');
// Which form the last refusal belongs to (its field is marked invalid).
const failedForm = ref<string | null>(null);

const chat = ref('');
const name = ref('');
const message = ref('');
const winner = ref<'' | (typeof winnerTeams)[number]>('');
const quakeSeconds = ref(30);
const timeOfDay = ref('');
const confirmingStop = ref(false);
const stopUnderstood = ref(false);
const stopButton = ref<HTMLButtonElement | null>(null);
const stopWarning = ref<HTMLElement | null>(null);

function serverLabel(): string {
  return props.server.server_name || 'the server';
}

function teamLabel(team: string): string {
  if (!team) return 'None';
  return TEAM_NAMES[team] ?? team;
}

// 'YYYY-MM-DD HH:MM:SS' UTC as an ISO instant.
function isoOf(utc: string): string {
  return `${utc.replace(' ', 'T')}Z`;
}

function localOf(utc: string): string {
  const t = Date.parse(isoOf(utc));
  return Number.isNaN(t) ? utc : new Date(t).toLocaleString();
}

function ago(utc: string): string {
  const t = Date.parse(isoOf(utc));
  if (Number.isNaN(t)) return utc;
  const seconds = Math.max(0, Math.round((props.now - t) / 1000));
  if (seconds < 60) return `${seconds} s ago`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)} min ago`;
  if (seconds < 86400) return `${Math.floor(seconds / 3600)} h ago`;
  return `${Math.floor(seconds / 86400)} d ago`;
}

function fail(form: string, error: HostError) {
  status.value = '';
  alertText.value = error.message;
  failedForm.value = form;
  if (error.code === 'unauthorized') emit('session-ended');
  if (error.code === 'unknown_rid' || error.code === 'not_hosting') emit('changed');
}

// One push; true once the service queued it.
async function push(form: string, body: ServerCommandBody, done: string): Promise<boolean> {
  if (busy.value) return false;
  busy.value = true;
  status.value = '';
  alertText.value = '';
  failedForm.value = null;
  const result = await sendServerCommand(props.server.rid, body);
  busy.value = false;
  if (!result.ok) {
    fail(form, result.error);
    return false;
  }
  status.value = done;
  emit('changed');
  return true;
}

async function sendChat() {
  const text = chat.value.trim();
  if (!text) return;
  if (await push('chat', { verb: 'TextChatServer', args: [text] }, `Sent to everyone on ${serverLabel()}: “${text}”.`)) {
    chat.value = '';
  }
}

async function sendName() {
  const text = name.value.trim();
  if (!text) return;
  if (await push('name', { verb: 'SetServerName', args: [text] }, `Asked ${serverLabel()} to rename itself “${text}”.`)) {
    name.value = '';
  }
}

async function sendMessage() {
  const text = message.value.trim();
  if (!text) return;
  if (await push('msg', { verb: 'SetServerMsg', args: [text] }, `Asked ${serverLabel()} to set its message to “${text}”.`)) {
    message.value = '';
  }
}

async function endMission(verb: 'Cycle' | 'EndMission' | 'GameOver') {
  const label = { Cycle: 'Move to the next map', EndMission: 'End the mission', GameOver: 'End the game' }[verb];
  const outcome = winner.value ? `${winner.value} wins.` : 'No team wins.';
  if (!window.confirm(`${label} on ${serverLabel()}? The running mission ends for everyone playing. ${outcome}`)) return;
  await push('mission', { verb, args: winner.value ? [winner.value] : [] }, `${label}: sent to ${serverLabel()}.`);
}

async function sendQuake() {
  const seconds = Math.min(40, Math.max(0, Math.round(Number(quakeSeconds.value) || 0)));
  await push('quake', { verb: 'Earthquake', args: [String(seconds)] }, `Sent a ${seconds}-second earthquake to ${serverLabel()}.`);
}

async function sendLightning() {
  await push('quake', { verb: 'Lightning' }, `Sent lightning to ${serverLabel()}.`);
}

async function sendTimeOfDay() {
  const hhmm = timeOfDay.value.trim();
  if (!/^([01][0-9]|2[0-3])[0-5][0-9]$/.test(hhmm)) {
    alertText.value = 'Enter the time as four digits, HHMM (0000 to 2359).';
    failedForm.value = 'tod';
    return;
  }
  if (await push('tod', { verb: 'TimeOfDay', args: [hhmm] }, `Set the time of day on ${serverLabel()} to ${hhmm}.`)) {
    timeOfDay.value = '';
  }
}

async function playerAction(verb: Extract<OwnerVerb, 'PuntPlayer' | 'KillPlayer' | 'ChangeTeam'>, slot: RosterSlot) {
  const who = slot.player_name || `slot ${slot.slot}`;
  const prompts: Record<typeof verb, string> = {
    PuntPlayer:
      slot.slot === 0
        ? `Kick ${who}? Slot 0 is a listen host's own player: kicking it ends the hosting for everyone.`
        : `Kick ${who} from ${serverLabel()}?`,
    KillPlayer: `Kill ${who}?`,
    ChangeTeam: `Move ${who} to the other team?`,
  };
  if (!window.confirm(prompts[verb])) return;
  const done: Record<typeof verb, string> = {
    PuntPlayer: `Asked ${serverLabel()} to kick ${who}.`,
    KillPlayer: `Asked ${serverLabel()} to kill ${who}.`,
    ChangeTeam: `Asked ${serverLabel()} to move ${who} to the other team.`,
  };
  await push(`player-${slot.slot}`, { verb, target: 'ByIndex', args: [String(slot.slot)] }, done[verb]);
}

async function beginStop() {
  confirmingStop.value = true;
  stopUnderstood.value = false;
  await nextTick();
  stopWarning.value?.focus();
}

async function cancelStop() {
  confirmingStop.value = false;
  stopUnderstood.value = false;
  await nextTick();
  stopButton.value?.focus();
}

async function confirmStop() {
  if (busy.value || !stopUnderstood.value) return;
  busy.value = true;
  status.value = '';
  alertText.value = '';
  failedForm.value = null;
  const result = await stopHosting(props.server.rid);
  busy.value = false;
  if (!result.ok) {
    fail('stop', result.error);
    return;
  }
  confirmingStop.value = false;
  emit('stopped', props.server.server_name);
}
</script>
