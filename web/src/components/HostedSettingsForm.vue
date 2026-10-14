<template>
  <form
    class="space-y-8 rounded-panel border border-border bg-panel p-4 sm:p-6"
    novalidate
    :aria-labelledby="`${baseId}-heading`"
    @submit.prevent="submit"
  >
    <div class="space-y-2">
      <h2 :id="`${baseId}-heading`" class="text-2xl font-semibold text-ink">
        {{ retry ? 'Start your server again' : 'Start your hosted server' }}
      </h2>
      <p class="text-sm text-ink-muted">
        {{
          retry
            ? 'Your last server failed. Check its settings and try again.'
            : 'Pick your settings and start. Your server stays in the server browser until you stop it.'
        }}
      </p>
    </div>

    <!-- Live region, always in the form so a screen reader hears what lands in it. -->
    <p role="alert" class="sr-only">{{ alertText }}</p>
    <p
      v-if="alertText"
      :id="`${baseId}-alert`"
      aria-hidden="true"
      class="rounded-panel border border-danger/40 bg-danger/10 p-3 text-sm text-danger"
    >
      {{ alertText }}
    </p>

    <div class="grid gap-6 sm:grid-cols-2">
      <!-- GameName -->
      <div class="min-w-0 space-y-1 sm:col-span-2">
        <label :for="fieldId('game_name')" :class="labelClass">{{ FIELD_LABELS.game_name }}</label>
        <input
          :id="fieldId('game_name')"
          v-model="form.game_name"
          type="text"
          autocomplete="off"
          :disabled="busy"
          :maxlength="GAME_NAME_MAX * 2"
          :aria-invalid="errors.game_name ? 'true' : undefined"
          :aria-describedby="describedBy('game_name')"
          :class="inputClass"
        />
        <p :id="hintId('game_name')" class="text-xs text-ink-muted">
          {{ cp1252Length(form.game_name.trim()) }} of {{ GAME_NAME_MAX }} characters. Players see it in the
          server browser. Accented letters work; emoji, most other scripts and double quotes do not.
        </p>
        <p v-if="errors.game_name" :id="errorId('game_name')" class="text-xs text-danger">{{ errors.game_name }}</p>
      </div>

      <!-- MPHostGamePassword -->
      <div class="min-w-0 space-y-1 sm:col-span-2">
        <label :for="fieldId('game_password')" :class="labelClass">{{ FIELD_LABELS.game_password }} (optional)</label>
        <div class="flex gap-2">
          <input
            :id="fieldId('game_password')"
            v-model="form.game_password"
            :type="showPassword ? 'text' : 'password'"
            autocomplete="off"
            autocapitalize="none"
            spellcheck="false"
            :disabled="busy"
            :maxlength="GAME_PASSWORD_MAX * 2"
            :aria-invalid="errors.game_password ? 'true' : undefined"
            :aria-describedby="describedBy('game_password')"
            :class="[inputBase, 'min-w-0 flex-1 font-mono']"
          />
          <button
            type="button"
            class="shrink-0 rounded-control border border-border bg-raised px-3 py-2 text-xs text-ink transition hover:bg-hover"
            :aria-pressed="showPassword ? 'true' : 'false'"
            :aria-controls="fieldId('game_password')"
            @click="showPassword = !showPassword"
          >
            {{ showPassword ? 'Hide' : 'Show' }}<span class="sr-only"> the game password</span>
          </button>
        </div>
        <p :id="hintId('game_password')" class="text-xs text-ink-muted">
          Leave it empty for a public server. Players type it to join. Up to {{ GAME_PASSWORD_MAX }} letters,
          digits and ASCII punctuation, with no space or double quote.
        </p>
        <p v-if="errors.game_password" :id="errorId('game_password')" class="text-xs text-danger">
          {{ errors.game_password }}
        </p>
      </div>

      <!-- MaxPlayers, Time_Limit, KillLimit, MaxScore, Respawn, Delay -->
      <div v-for="spec in NUMBER_FIELDS" :key="spec.field" class="min-w-0 space-y-1">
        <label :for="fieldId(spec.field)" :class="labelClass">{{ FIELD_LABELS[spec.field] }}</label>
        <div class="flex items-center gap-2">
          <input
            :id="fieldId(spec.field)"
            v-model.number="form[spec.field]"
            type="number"
            inputmode="numeric"
            step="1"
            :min="INT_RULES[spec.field].min"
            :max="INT_RULES[spec.field].max"
            :disabled="busy"
            :aria-invalid="errors[spec.field] ? 'true' : undefined"
            :aria-describedby="describedBy(spec.field)"
            :class="[inputBase, 'w-28']"
          />
          <span v-if="spec.unit" class="text-sm text-ink-muted">{{ spec.unit }}</span>
        </div>
        <p :id="hintId(spec.field)" class="text-xs text-ink-muted">
          {{ INT_RULES[spec.field].min }} to {{ INT_RULES[spec.field].max }}.{{ spec.hint ? ` ${spec.hint}` : '' }}
        </p>
        <p v-if="errors[spec.field]" :id="errorId(spec.field)" class="text-xs text-danger">{{ errors[spec.field] }}</p>
      </div>

      <!-- TeamFF -->
      <div class="min-w-0 space-y-1 sm:col-span-2">
        <label class="flex items-center gap-3 text-sm text-ink">
          <input
            :id="fieldId('team_ff')"
            v-model="form.team_ff"
            type="checkbox"
            :disabled="busy"
            :aria-describedby="hintId('team_ff')"
            class="h-4 w-4 rounded border-border bg-surface text-accent focus:ring-accent"
          />
          {{ FIELD_LABELS.team_ff }}
        </label>
        <p :id="hintId('team_ff')" class="text-xs text-ink-muted">Teammates can hurt each other.</p>
      </div>
    </div>

    <!-- /exp jox01 -->
    <fieldset class="min-w-0 space-y-2" :aria-describedby="describedBy('expansion')">
      <legend
        :id="fieldId('expansion')"
        tabindex="-1"
        :class="[labelClass, 'focus:outline-none']"
      >
        {{ FIELD_LABELS.expansion }}
      </legend>
      <div class="flex flex-col gap-2 sm:flex-row sm:gap-6">
        <label v-for="option in EXPANSIONS" :key="option.value" class="flex items-center gap-3 text-sm text-ink">
          <input
            v-model="form.expansion"
            type="radio"
            :name="`${baseId}-expansion`"
            :value="option.value"
            :disabled="busy"
            class="h-4 w-4 border-border bg-surface text-accent focus:ring-accent"
          />
          {{ option.label }}
        </label>
      </div>
      <p :id="hintId('expansion')" class="text-xs text-ink-muted">
        Players need the same expansion to join: a player without Escalation installed cannot join an
        Escalation server.
      </p>
      <p v-if="errors.expansion" :id="errorId('expansion')" class="text-xs text-danger">{{ errors.expansion }}</p>
    </fieldset>

    <p
      v-if="droppedMissions.length > 0"
      class="rounded-panel border border-warn/40 bg-warn/10 p-3 text-sm text-warn"
    >
      Your last server's rotation had
      {{ droppedMissions.length === 1 ? 'a mission' : `${droppedMissions.length} missions` }} this service no
      longer offers, so {{ droppedMissions.length === 1 ? 'it was' : 'they were' }} left out:
      <span class="break-all font-mono text-xs">{{ droppedMissions.join(', ') }}</span>.
    </p>

    <MissionRotationPicker
      ref="rotation"
      v-model="form.missions"
      :available="defaultMissions"
      :base-id="`${baseId}-missions`"
      :error="errors.missions"
      :disabled="busy"
    />

    <div class="flex flex-col gap-3 border-t border-border pt-6 sm:flex-row sm:items-center sm:justify-between">
      <p class="text-xs text-ink-muted">Hosted servers are experimental (pre-1.0).</p>
      <button
        type="submit"
        :disabled="busy"
        class="rounded-control bg-accent px-6 py-3 font-semibold text-on-accent transition hover:bg-accent/90 disabled:cursor-not-allowed disabled:opacity-50"
      >
        {{ busy ? 'Starting…' : retry ? 'Try again' : 'Start server' }}
      </button>
    </div>
  </form>
</template>

<script setup lang="ts">
// The hosted server's settings and its start (POST /api/me/server/start):
// the My servers page's one start control, offered only while the account
// has no live server (the service allows one: 409 already_live). After a
// failed server it is the owner's try again, with that server's settings.
// Every field is checked against the backend's rule (utils/hostedSettings.ts)
// before the request; a refusal the backend gives on one field
// (invalid_settings) marks that field with the rule, never the backend's text.
import { computed, nextTick, reactive, ref, watch } from 'vue';
import MissionRotationPicker from './MissionRotationPicker.vue';
import { startHostedServer, type HostedError } from '../api/hostedServer';
import type { HostedExpansion, HostedServer, HostedSettings } from '../types/hostedServer';
import { cp1252Length } from '../utils/cp1252';
import {
  checkSettings,
  FIELD_LABELS,
  FIELD_RULES,
  formFrom,
  GAME_NAME_MAX,
  GAME_PASSWORD_MAX,
  INT_RULES,
  isMissionName,
  uniqueMissions,
  type IntField,
  type SettingsErrors,
  type SettingsField,
} from '../utils/hostedSettings';

const props = defineProps<{
  // The account's last server's settings, to start it again; null for none.
  previous: HostedSettings | null;
  // The service's missions (GET /api/me/server default_missions).
  defaultMissions: string[];
  // The last server failed: the start is another try at it.
  retry?: boolean;
}>();

const emit = defineEmits<{
  // The service took the request (202); the server is `requested`.
  (e: 'started', server: HostedServer | null): void;
  // A refusal the page acts on besides showing it here (a session that ended,
  // a server already live, the approval or hosting gone).
  (e: 'refused', error: HostedError): void;
}>();

const baseId = 'hosted-settings';
const labelClass = 'block text-xs font-medium uppercase tracking-wider text-ink-muted';
const inputBase =
  'rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none disabled:opacity-60 aria-[invalid=true]:border-danger';
const inputClass = `w-full ${inputBase}`;

const NUMBER_FIELDS: { field: IntField; unit?: string; hint?: string }[] = [
  { field: 'max_players', unit: 'players' },
  { field: 'time_limit', unit: 'minutes' },
  { field: 'kill_limit', hint: '500 means no limit.' },
  { field: 'max_score', hint: '500 means no limit.' },
  { field: 'respawn', unit: 'seconds' },
  { field: 'start_delay', unit: 'seconds', hint: 'The wait before each round starts.' },
];

const EXPANSIONS: { value: HostedExpansion; label: string }[] = [
  { value: '', label: 'Joint Operations (base game)' },
  { value: 'jox01', label: 'Escalation (jox01)' },
];

// The fields in the order the page shows them: the first refused one takes
// the keyboard.
const FIELD_ORDER: SettingsField[] = [
  'game_name',
  'game_password',
  'max_players',
  'time_limit',
  'kill_limit',
  'max_score',
  'respawn',
  'start_delay',
  'team_ff',
  'expansion',
  'missions',
];

// The rotation to begin with: the last server's, each mission once (the
// picker holds each at most once) and kept to the service's list when it has
// one, else the service's own. `dropped` names the last server's missions the
// service no longer offers (or names no .bms file), which the form says it left
// out.
function initialMissions(): { missions: string[]; dropped: string[] } {
  const offered = uniqueMissions(props.defaultMissions);
  const previous = uniqueMissions(props.previous?.missions ?? []);
  if (offered.length === 0) {
    return { missions: previous.filter(isMissionName), dropped: previous.filter((m) => !isMissionName(m)) };
  }
  const known = new Map(offered.map((m) => [m.toLowerCase(), m] as const));
  const kept: string[] = [];
  const dropped: string[] = [];
  for (const m of previous) {
    const name = known.get(m.toLowerCase());
    if (name) kept.push(name);
    else dropped.push(m);
  }
  return { missions: kept.length > 0 ? kept : [...offered], dropped };
}

const initial = initialMissions();
// The last server's missions the rotation left out, said once above the picker.
const droppedMissions = initial.dropped;
const form = reactive(formFrom(props.previous, initial.missions));
const busy = ref(false);
// The game password is masked (it may come from the last server) until shown.
const showPassword = ref(false);
const alertText = ref('');
// The form's own check, once a start was tried (then again on every change).
const checked = ref<SettingsErrors>({});
const attempted = ref(false);
// The field the backend refused, with its rule, until that field changes.
const serverRefusal = ref<{ field: SettingsField; message: string } | null>(null);
const rotation = ref<{ focus: () => void } | null>(null);

const errors = computed<SettingsErrors>(() => {
  const out: SettingsErrors = { ...checked.value };
  const refusal = serverRefusal.value;
  if (refusal && !out[refusal.field]) out[refusal.field] = refusal.message;
  return out;
});

watch(
  form,
  () => {
    if (attempted.value) checked.value = checkSettings(form).errors;
  },
  { deep: true },
);

// A change to the field the backend refused takes its mark away.
const refusedValue = computed(() => {
  const field = serverRefusal.value?.field;
  return field ? JSON.stringify(form[field]) : '';
});
watch(refusedValue, (now, before) => {
  if (before !== '' && now !== before) serverRefusal.value = null;
});

function fieldId(field: SettingsField): string {
  return `${baseId}-${field}`;
}
function hintId(field: SettingsField): string {
  return `${fieldId(field)}-hint`;
}
function errorId(field: SettingsField): string {
  return `${fieldId(field)}-error`;
}
function describedBy(field: SettingsField): string {
  return errors.value[field] ? `${hintId(field)} ${errorId(field)}` : hintId(field);
}

async function focusFirstError(errs: SettingsErrors) {
  await nextTick();
  const first = FIELD_ORDER.find((f) => errs[f]);
  if (!first) return;
  if (first === 'missions') {
    rotation.value?.focus();
    return;
  }
  document.getElementById(fieldId(first))?.focus();
}

async function submit() {
  if (busy.value) return;
  attempted.value = true;
  serverRefusal.value = null;
  const result = checkSettings(form);
  checked.value = result.errors;
  if (!result.settings) {
    const count = Object.keys(result.errors).length;
    alertText.value = `Check the ${count === 1 ? 'setting' : `${count} settings`} marked below.`;
    await focusFirstError(result.errors);
    return;
  }
  alertText.value = '';
  busy.value = true;
  const reply = await startHostedServer(result.settings);
  busy.value = false;
  if (reply.ok) {
    emit('started', reply.value);
    return;
  }
  const error = reply.error;
  alertText.value = error.message;
  if (error.code === 'invalid_settings' && error.field) {
    const refused: SettingsErrors = {};
    refused[error.field] = FIELD_RULES[error.field];
    serverRefusal.value = { field: error.field, message: FIELD_RULES[error.field] };
    await focusFirstError(refused);
  }
  emit('refused', error);
}
</script>
