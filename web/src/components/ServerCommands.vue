<template>
  <div class="server-commands space-y-6">
    <LiveNotices :message="status" :error="alertText" />

    <!-- SetServerName / SetServerMsg -->
    <section class="space-y-2" :aria-labelledby="`${idBase}-server`">
      <h2 :id="`${idBase}-server`" class="section-heading">Name and message</h2>
      <form class="space-y-1" @submit.prevent="sendName">
        <label :for="`${idBase}-name`" class="field-label">Name <span class="font-normal">({{ cp1252Length(name) }}/31)</span></label>
        <div class="flex gap-2">
          <input
            :id="`${idBase}-name`"
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
      <form class="space-y-1" @submit.prevent="sendMessage">
        <label :for="`${idBase}-msg`" class="field-label">Message <span class="font-normal">({{ cp1252Length(message) }}/127)</span></label>
        <div class="flex gap-2">
          <input
            :id="`${idBase}-msg`"
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
    </section>

    <!-- Cycle / EndMission / GameOver -->
    <section class="space-y-2" :aria-labelledby="`${idBase}-mission`">
      <h2 :id="`${idBase}-mission`" class="section-heading">Mission</h2>
      <div class="flex flex-wrap items-center gap-2">
        <label :for="`${idBase}-winner`" class="sr-only">Winner</label>
        <select :id="`${idBase}-winner`" v-model="winner" class="field w-auto">
          <option value="">No winner</option>
          <option v-for="team in WINNER_TEAMS" :key="team" :value="team">{{ team }} wins</option>
        </select>
        <button type="button" class="btn-warn btn-sm" :disabled="busy" @click="endMission('Cycle')">Next map</button>
        <button type="button" class="btn-warn btn-sm" :disabled="busy" @click="endMission('EndMission')">End mission</button>
        <button type="button" class="btn-warn btn-sm" :disabled="busy" @click="endMission('GameOver')">Game over</button>
      </div>
    </section>

    <!-- Earthquake / Lightning / TimeOfDay -->
    <section class="space-y-2" :aria-labelledby="`${idBase}-weather`">
      <h2 :id="`${idBase}-weather`" class="section-heading">Weather and time</h2>
      <div class="flex flex-wrap items-center gap-2">
        <form class="flex items-center gap-2" @submit.prevent="sendQuake">
          <label :for="`${idBase}-quake`" class="sr-only">Earthquake seconds</label>
          <input :id="`${idBase}-quake`" v-model.number="quakeSeconds" type="number" min="0" max="40" required class="field w-20" title="Seconds, up to 40" />
          <button type="submit" class="btn-secondary btn-sm" :disabled="busy">Shake</button>
        </form>
        <button type="button" class="btn-secondary btn-sm" :disabled="busy" @click="sendLightning">Lightning</button>
        <form class="flex items-center gap-2" @submit.prevent="sendTimeOfDay">
          <label :for="`${idBase}-time-of-day`" class="sr-only">Time of day</label>
          <input
            :id="`${idBase}-time-of-day`"
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
    </section>
  </div>
</template>

<script setup lang="ts">
// An owner's pushes to a listed server of theirs (POST /api/me/hosts/<rid>/
// command): its name and message, the mission's end with a winner, weather
// and time. A game the account hosts from the game shows them on its page; a
// hosted server on its Console tab, beside the console's chat and players.
import { computed, ref, watch } from 'vue';
import LiveNotices from './ui/LiveNotices.vue';
import { sendServerCommand, type HostError } from '../api/myServers';
import { confirmAction } from '../composables/useConfirm';
import type { MyServer, ServerCommandBody } from '../types/myServers';
import { cp1252Length, cp1252Problem } from '../utils/cp1252';
import { serverName } from '../utils/games';
import { stripGameText } from '../utils/gameText';

// The winner tokens the Cycle / EndMission / GameOver reader takes (a team
// name or its number; none means no winner).
const WINNER_TEAMS = ['Blue', 'Red', 'Yellow', 'Violet'] as const;

const props = withDefaults(defineProps<{ server: MyServer; idBase?: string }>(), { idBase: 'server-commands' });
const emit = defineEmits<{
  // A push was queued: the listing changes shortly.
  (e: 'changed'): void;
  // A refusal the page acts on: the session ended, or the listing is gone.
  (e: 'refused', error: HostError): void;
}>();

const plainTitle = computed(() => stripGameText(serverName(props.server.server_name)));

const busy = ref(false);
const status = ref('');
const alertText = ref('');
// The form whose field the last refusal was about (a 400 invalid_command, or
// the page's own check): its field is marked invalid.
const failedForm = ref<string | null>(null);

// The server's own name and message, which the fields follow until edited.
const name = ref(props.server.server_name);
const message = ref(props.server.msg);
watch(
  () => props.server.server_name,
  (now, before) => {
    if (name.value === (before ?? '')) name.value = now;
  },
);
watch(
  () => props.server.msg,
  (now, before) => {
    if (message.value === (before ?? '')) message.value = now;
  },
);
const winner = ref<'' | (typeof WINNER_TEAMS)[number]>('');
const quakeSeconds = ref(30);
const timeOfDay = ref('');

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
    alertText.value = result.error.message;
    failedForm.value = result.error.code === 'invalid_command' ? form : null;
    emit('refused', result.error);
    return false;
  }
  status.value = done;
  emit('changed');
  return true;
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
  if (!(await confirmAction(`${label} on ${plainTitle.value}? The mission ends for everyone${outcome}.`))) return;
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
</script>
