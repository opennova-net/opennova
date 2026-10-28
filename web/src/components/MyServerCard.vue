<template>
  <component
    :is="embedded ? 'div' : 'article'"
    :class="embedded ? 'space-y-4' : 'card space-y-4 p-4 sm:p-5'"
    :aria-labelledby="embedded ? undefined : ids.name"
  >
    <header v-if="!embedded" class="flex flex-col gap-2 sm:flex-row sm:items-start sm:justify-between">
      <div class="min-w-0 space-y-1">
        <h3 :id="ids.name" class="text-base">
          <ServerLink :name="server.server_name" :rid="server.rid" scope="public" />
        </h3>
        <p class="flex flex-wrap items-center gap-x-3 gap-y-1 text-sm text-ink-muted">
          <span class="text-ink">{{ server.mission_name || 'No mission yet' }}</span>
          <span v-if="gameTypeName(server.game_type)"><GameText :text="server.game_type" /></span>
          <span>{{ server.players }} of {{ server.max_players }} players</span>
          <span class="font-mono text-xs">{{ server.host_ip }}:{{ server.host_port }}</span>
          <span v-if="server.msg" class="break-words">“{{ server.msg }}”</span>
        </p>
      </div>
      <p class="shrink-0 text-xs text-ink-muted">Updated <RelativeTime :value="server.updated_at" :now="now" /></p>
    </header>

    <LiveNotices :message="status" :error="alertText" />

    <div class="grid gap-4 lg:grid-cols-2">
      <!-- TextChatServer (a hosted server's chat is its console's) -->
      <form v-if="!embedded" class="space-y-1" @submit.prevent="sendChat">
        <label :for="ids.chat" class="field-label">Say to everyone</label>
        <div class="flex gap-2">
          <input
            :id="ids.chat"
            v-model="chat"
            type="text"
            class="field min-w-0 flex-1"
            required
            maxlength="400"
            autocomplete="off"
            :aria-invalid="failedForm === 'chat' ? 'true' : undefined"
          />
          <button type="submit" class="btn-secondary" :disabled="busy">Send</button>
        </div>
      </form>

      <!-- SetServerName -->
      <form class="space-y-1" @submit.prevent="sendName">
        <label :for="ids.serverName" class="field-label">Server name <span class="font-normal">({{ cp1252Length(name) }}/31)</span></label>
        <div class="flex gap-2">
          <input
            :id="ids.serverName"
            v-model="name"
            type="text"
            class="field min-w-0 flex-1"
            required
            maxlength="31"
            autocomplete="off"
            :aria-invalid="failedForm === 'name' ? 'true' : undefined"
          />
          <button type="submit" class="btn-secondary" :disabled="busy">Rename</button>
        </div>
      </form>

      <!-- SetServerMsg -->
      <form class="space-y-1" @submit.prevent="sendMessage">
        <label :for="ids.serverMsg" class="field-label">Server message <span class="font-normal">({{ cp1252Length(message) }}/127)</span></label>
        <div class="flex gap-2">
          <input
            :id="ids.serverMsg"
            v-model="message"
            type="text"
            class="field min-w-0 flex-1"
            required
            maxlength="127"
            autocomplete="off"
            :aria-invalid="failedForm === 'msg' ? 'true' : undefined"
          />
          <button type="submit" class="btn-secondary" :disabled="busy">Set</button>
        </div>
      </form>

      <!-- Cycle / EndMission / GameOver -->
      <fieldset class="space-y-1">
        <legend class="field-label">Mission</legend>
        <div class="flex flex-wrap items-center gap-2">
          <label :for="ids.winner" class="sr-only">Winner</label>
          <select :id="ids.winner" v-model="winner" class="field w-auto">
            <option value="">No winner</option>
            <option v-for="team in winnerTeams" :key="team" :value="team">{{ team }} wins</option>
          </select>
          <button type="button" class="btn-warn btn-sm" :disabled="busy" @click="endMission('Cycle')">Next map</button>
          <button type="button" class="btn-warn btn-sm" :disabled="busy" @click="endMission('EndMission')">End mission</button>
          <button type="button" class="btn-warn btn-sm" :disabled="busy" @click="endMission('GameOver')">Game over</button>
        </div>
      </fieldset>

      <!-- Earthquake / Lightning / TimeOfDay -->
      <fieldset class="space-y-1">
        <legend class="field-label">Weather and time</legend>
        <div class="flex flex-wrap items-center gap-2">
          <form class="flex items-center gap-2" @submit.prevent="sendQuake">
            <label :for="ids.quake" class="sr-only">Earthquake seconds</label>
            <input :id="ids.quake" v-model.number="quakeSeconds" type="number" min="0" max="40" required class="field w-20" title="Seconds, up to 40" />
            <button type="submit" class="btn-secondary btn-sm" :disabled="busy">Shake</button>
          </form>
          <button type="button" class="btn-secondary btn-sm" :disabled="busy" @click="sendLightning">Lightning</button>
          <form class="flex items-center gap-2" @submit.prevent="sendTimeOfDay">
            <label :for="ids.timeOfDay" class="sr-only">Time of day</label>
            <input
              :id="ids.timeOfDay"
              v-model="timeOfDay"
              type="text"
              inputmode="numeric"
              required
              pattern="([01][0-9]|2[0-3])[0-5][0-9]"
              maxlength="4"
              placeholder="HHMM"
              title="24-hour time, such as 1330"
              :aria-invalid="failedForm === 'tod' ? 'true' : undefined"
              class="field w-24 font-mono"
            />
            <button type="submit" class="btn-secondary btn-sm" :disabled="busy">Set time</button>
          </form>
        </div>
      </fieldset>
    </div>

    <!-- Players: PuntPlayer / KillPlayer / ChangeTeam, aimed ByIndex at a roster
         slot (a hosted server's players are its console's) -->
    <div v-if="!embedded" class="table-wrap">
      <table class="data-table">
        <caption class="sr-only">Players on {{ server.server_name }}</caption>
        <thead>
          <tr>
            <th scope="col">Slot</th>
            <th scope="col">Player</th>
            <th scope="col">Team</th>
            <th scope="col"><span class="sr-only">Actions</span></th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="slot in server.roster" :key="slot.slot">
            <td class="font-mono text-xs">{{ slot.slot }}</td>
            <th scope="row">{{ slot.player_name || '(unnamed)' }}</th>
            <td>{{ teamLabel(slot.team) }}</td>
            <td class="whitespace-nowrap text-right">
              <button type="button" class="btn-ghost btn-sm" :disabled="busy" @click="playerAction('ChangeTeam', slot)">
                Switch team<span class="sr-only">, {{ slot.player_name }}</span>
              </button>
              <button type="button" class="btn-ghost btn-sm" :disabled="busy" @click="playerAction('KillPlayer', slot)">
                Kill<span class="sr-only">, {{ slot.player_name }}</span>
              </button>
              <button type="button" class="btn-ghost btn-sm" :disabled="busy" @click="playerAction('PuntPlayer', slot)">
                Kick<span class="sr-only">, {{ slot.player_name }}</span>
              </button>
            </td>
          </tr>
          <TableState v-if="server.roster.length === 0" :colspan="4" :rows="server.roster" empty="No players." />
        </tbody>
      </table>
    </div>

    <!-- ServerStopHosting (MsgCode 7); a hosted server's stop is its page's -->
    <div v-if="!embedded" class="flex justify-end">
      <button type="button" class="btn-danger btn-sm" :disabled="busy" @click="stop">Stop hosting</button>
    </div>
  </component>
</template>

<script setup lang="ts">
// One of the account's listed servers with the owner's pushes to it
// (api/myServers.ts), on the account's Servers page. `embedded` puts it on a
// hosted server's own page for that server's listing, with only what its
// console lacks: the name, the message, the mission's end and the weather and
// time. Its header, chat, players and stop are then the page's own.
import { computed, ref, watch } from 'vue';
import GameText from './GameText.vue';
import RelativeTime from './RelativeTime.vue';
import ServerLink from './links/ServerLink.vue';
import LiveNotices from './ui/LiveNotices.vue';
import TableState from './ui/TableState.vue';
import { sendServerCommand, stopHosting, type HostError } from '../api/myServers';
import { confirmAction } from '../composables/useConfirm';
import type { MyServer, OwnerVerb, RosterSlot, ServerCommandBody } from '../types/myServers';
import { cp1252Length, cp1252Problem } from '../utils/cp1252';
import { gameTypeName } from '../utils/games';

const props = defineProps<{
  server: MyServer;
  // The page's clock (ms), for the "ago" times.
  now: number;
  // Inside the hosted server's page: the controls its console lacks alone.
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

// The ids the labels point at, unique per server.
const ids = computed(() => {
  const base = `server-${props.server.rid}`;
  return {
    name: `${base}-name`,
    chat: `${base}-chat`,
    serverName: `${base}-server-name`,
    serverMsg: `${base}-server-msg`,
    winner: `${base}-winner`,
    quake: `${base}-quake`,
    timeOfDay: `${base}-time-of-day`,
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
// the page's own check): its field is marked invalid.
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

// The page's own check of a text before it is sent: false, with the reason
// shown and the field marked, when it fails.
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
  if (await push('chat', { verb: 'TextChatServer', args: [text] }, `Sent: “${text}”.`)) chat.value = '';
}

async function sendName() {
  const text = name.value.trim();
  if (!text || !textFits('name', text, 31)) return;
  await push('name', { verb: 'SetServerName', args: [text] }, `Renaming to “${text}”.`);
}

async function sendMessage() {
  const text = message.value.trim();
  if (!text || !textFits('msg', text, 127)) return;
  await push('msg', { verb: 'SetServerMsg', args: [text] }, `Message set to “${text}”.`);
}

async function endMission(verb: 'Cycle' | 'EndMission' | 'GameOver') {
  const label = { Cycle: 'Next map', EndMission: 'End the mission', GameOver: 'End the game' }[verb];
  const outcome = winner.value ? `; ${winner.value} wins` : '';
  if (!(await confirmAction(`${label} on ${serverLabel()}? The mission ends for everyone${outcome}.`))) return;
  await push('mission', { verb, args: winner.value ? [winner.value] : [] }, `${label}: sent.`);
}

async function sendQuake() {
  const seconds = Math.min(40, Math.max(0, Math.round(Number(quakeSeconds.value) || 0)));
  await push('quake', { verb: 'Earthquake', args: [String(seconds)] }, `Sent a ${seconds}-second earthquake.`);
}

async function sendLightning() {
  await push('quake', { verb: 'Lightning' }, 'Sent lightning.');
}

async function sendTimeOfDay() {
  const hhmm = timeOfDay.value.trim();
  if (!/^([01][0-9]|2[0-3])[0-5][0-9]$/.test(hhmm)) {
    alertText.value = 'Enter the time as four digits, HHMM (0000 to 2359).';
    failedForm.value = 'tod';
    return;
  }
  if (await push('tod', { verb: 'TimeOfDay', args: [hhmm] }, `Time of day set to ${hhmm}.`)) timeOfDay.value = '';
}

async function playerAction(verb: Extract<OwnerVerb, 'PuntPlayer' | 'KillPlayer' | 'ChangeTeam'>, slot: RosterSlot) {
  const who = slot.player_name || `slot ${slot.slot}`;
  const expectedName = slot.player_name;
  const serverId = props.server.rid;
  const prompts: Record<typeof verb, string> = {
    PuntPlayer:
      slot.slot === 0
        ? `Kick ${who}? Slot 0 is the host's own player: the hosting ends for everyone.`
        : `Kick ${who} from ${serverLabel()}?`,
    KillPlayer: `Kill ${who}?`,
    ChangeTeam: `Move ${who} to the other team?`,
  };
  if (!(await confirmAction(prompts[verb]))) return;
  const current = props.server.roster.find((entry) => entry.slot === slot.slot);
  if (props.server.rid !== serverId || !current || current.player_name !== expectedName) {
    status.value = '';
    failedForm.value = null;
    alertText.value = 'The player list changed. Choose a player from the current list and try again.';
    return;
  }
  const done: Record<typeof verb, string> = {
    PuntPlayer: `Kicking ${who}.`,
    KillPlayer: `Killing ${who}.`,
    ChangeTeam: `Moving ${who} to the other team.`,
  };
  await push(`player-${slot.slot}`, { verb, target: 'ByIndex', args: [String(slot.slot)] }, done[verb]);
}

async function stop() {
  if (busy.value) return;
  if (!(await confirmAction(`Stop hosting ${serverLabel()}? Its match ends for everyone.`))) return;
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
  // The page drops this card and takes the keyboard to its own heading.
  emit('stopped', props.server.rid, props.server.server_name);
}
</script>
