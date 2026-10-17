<template>
  <component
    :is="embedded ? 'div' : 'article'"
    :class="embedded ? 'space-y-6' : 'space-y-6 rounded-panel border border-border bg-panel p-6'"
    :aria-labelledby="embedded ? undefined : ids.name"
  >
    <header v-if="!embedded" class="flex flex-col gap-3 sm:flex-row sm:items-start sm:justify-between">
      <div class="min-w-0">
        <h3 :id="ids.name" class="truncate text-2xl font-semibold text-ink">
          {{ server.server_name || 'Unnamed server' }}
        </h3>
        <p class="mt-1 text-sm text-ink-muted">
          <span class="text-ink">{{ server.mission_name || 'No mission yet' }}</span>
          <span v-if="server.game_type"> · {{ server.game_type }}</span>
          · {{ server.players }} of {{ server.max_players }} players
          · <span class="font-mono text-xs">{{ server.host_ip }}:{{ server.host_port }}</span>
        </p>
        <p v-if="server.msg" class="mt-1 text-sm text-ink-muted">
          Message: <span class="text-ink">{{ server.msg }}</span>
        </p>
      </div>
      <dl class="grid shrink-0 grid-cols-[auto_auto] gap-x-3 text-xs text-ink-muted sm:text-right">
        <dt>Listed</dt>
        <dd><RelativeTime :value="server.created_at" :now="now" /></dd>
        <dt>Updated</dt>
        <dd><RelativeTime :value="server.updated_at" :now="now" /></dd>
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
      <!-- TextChatServer (a hosted server's chat is its console's) -->
      <form v-if="!embedded" class="space-y-2" @submit.prevent="sendChat">
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
            :aria-invalid="failedForm === 'name' ? 'true' : undefined"
            :aria-describedby="failedForm === 'name' ? `${ids.nameHint} ${ids.error}` : ids.nameHint"
            class="min-w-0 flex-1 rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none"
          />
          <button type="submit" :disabled="busy" :class="primaryButton">Rename</button>
        </div>
        <p :id="ids.nameHint" class="text-xs text-ink-muted">
          {{ cp1252Length(name) }} of 31 characters. Accented letters work; emoji and most other
          scripts do not.
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
            :aria-invalid="failedForm === 'msg' ? 'true' : undefined"
            :aria-describedby="failedForm === 'msg' ? `${ids.msgHint} ${ids.error}` : ids.msgHint"
            class="min-w-0 flex-1 rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none"
          />
          <button type="submit" :disabled="busy" :class="primaryButton">Set</button>
        </div>
        <p :id="ids.msgHint" class="text-xs text-ink-muted">
          {{ cp1252Length(message) }} of 127 characters.
        </p>
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
          Each ends the mission for everyone on the server, with the winner you pick, and moves on to
          the next map.
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

    <!-- Players: PuntPlayer / KillPlayer / ChangeTeam, aimed ByIndex at a roster slot (a
         hosted server's players are its console's) -->
    <section v-if="!embedded" class="space-y-3" :aria-labelledby="ids.players">
      <h4 :id="ids.players" class="text-lg font-semibold text-ink">Players</h4>
      <p v-if="server.roster.length === 0" class="text-sm text-ink-muted">
        No players in the game, as of the server's last update.
      </p>
      <!-- relative: the rows' sr-only spans are absolutely positioned, and without a
           positioned ancestor they escape this scroller and widen the page on a phone. -->
      <div v-else class="relative overflow-x-auto rounded-panel border border-border">
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

    <!-- ServerStopHosting (MsgCode 7); a hosted server's stop is its section's -->
    <section v-if="!embedded" class="space-y-3 rounded-panel border border-danger/40 p-4" :aria-labelledby="ids.stop">
      <h4 :id="ids.stop" class="text-lg font-semibold text-danger">Stop hosting</h4>
      <p class="text-sm text-ink-muted">
        Ends the match for everyone on the server and takes it off NovaWorld.
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
  </component>
</template>

<script setup lang="ts">
// One of the account's listed servers with the owner's pushes to it
// (api/myServers.ts), on the account's Servers page. `embedded` puts it on a
// hosted server's own page (its Overview tab) for that server's listing, with
// only what its console lacks: the name, the message, the mission's end and
// the weather and time. Its header, chat, players and stop are then the
// page's own.
import { computed, nextTick, ref, watch } from 'vue';
import { sendServerCommand, stopHosting, type HostError } from '../api/myServers';
import type { MyServer, OwnerVerb, RosterSlot, ServerCommandBody } from '../types/myServers';
import { cp1252Length, cp1252Problem } from '../utils/cp1252';
import RelativeTime from './RelativeTime.vue';

const props = defineProps<{
  server: MyServer;
  // The page's clock (ms), for the "ago" times.
  now: number;
  // Inside the hosted server's section: the controls its console lacks alone.
  embedded?: boolean;
}>();

const emit = defineEmits<{
  // A push may change the listing (or showed it gone): refresh the list.
  (e: 'changed'): void;
  // The stop went out; the server leaves the list.
  (e: 'stopped', rid: number, name: string): void;
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
// The form whose field the last refusal was about (a 400 invalid_command, or
// the page's own check): its field is marked invalid. A 429, a 404 or a
// network failure says nothing about what was typed, so it marks none.
const failedForm = ref<string | null>(null);

const chat = ref('');
// The server's own name and message, which the fields follow until edited.
const name = ref(props.server.server_name);
const message = ref(props.server.msg);
watch(
  () => props.server.server_name,
  (now, before) => {
    if (name.value === before) name.value = now;
  },
);
watch(
  () => props.server.msg,
  (now, before) => {
    if (message.value === before) message.value = now;
  },
);
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

function fail(form: string, error: HostError) {
  status.value = '';
  alertText.value = error.message;
  failedForm.value = error.code === 'invalid_command' ? form : null;
  if (error.code === 'unauthorized') emit('session-ended');
  if (error.code === 'unknown_rid' || error.code === 'not_hosting') emit('changed');
}

// The page's own check of a text before it is sent (what the service would
// refuse): false, with the reason shown and the field marked, when it fails.
function textFits(form: string, text: string, cap?: number): boolean {
  const problem = cp1252Problem(text, cap);
  if (problem === null) return true;
  status.value = '';
  alertText.value = problem;
  failedForm.value = form;
  return false;
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
  if (!text || !textFits('chat', text)) return;
  if (await push('chat', { verb: 'TextChatServer', args: [text] }, `Sent to everyone on ${serverLabel()}: “${text}”.`)) {
    chat.value = '';
  }
}

async function sendName() {
  const text = name.value.trim();
  if (!text || !textFits('name', text, 31)) return;
  await push('name', { verb: 'SetServerName', args: [text] }, `Asked ${serverLabel()} to rename itself “${text}”.`);
}

async function sendMessage() {
  const text = message.value.trim();
  if (!text || !textFits('msg', text, 127)) return;
  await push('msg', { verb: 'SetServerMsg', args: [text] }, `Asked ${serverLabel()} to set its message to “${text}”.`);
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
  // The page drops this card and takes the keyboard to its own status.
  emit('stopped', props.server.rid, props.server.server_name);
}
</script>
