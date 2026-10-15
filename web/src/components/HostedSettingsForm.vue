<template>
  <form
    class="space-y-8 rounded-panel border border-border bg-panel p-4 sm:p-6"
    novalidate
    :aria-labelledby="`${baseId}-heading`"
    @submit.prevent="submit"
  >
    <div class="space-y-2">
      <h2 :id="`${baseId}-heading`" class="text-2xl font-semibold text-ink">
        {{ retry ? 'Start your server again' : 'Start a hosted server' }}
      </h2>
      <p class="text-sm text-ink-muted">
        {{
          retry
            ? 'That server failed. Check its settings and try again.'
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

    <!-- The data set: the game the server runs, and the lobby it lists in. -->
    <fieldset class="min-w-0 space-y-2" :aria-describedby="describedBy('game')">
      <legend :id="fieldId('game')" tabindex="-1" :class="[labelClass, 'focus:outline-none']">
        {{ FIELD_LABELS.game }}
      </legend>
      <div class="flex flex-col gap-2 sm:flex-row sm:flex-wrap sm:gap-6">
        <label v-for="option in dataSets" :key="option.game" class="flex items-center gap-3 text-sm text-ink">
          <input
            v-model="form.game"
            type="radio"
            :name="`${baseId}-game`"
            :value="option.game"
            :disabled="busy"
            class="h-4 w-4 border-border bg-surface text-accent focus:ring-accent"
          />
          {{ option.display_name }}
        </label>
      </div>
      <p :id="hintId('game')" class="text-xs text-ink-muted">
        Each game lists in its own server browser: players join with that game.
      </p>
      <p v-if="errors.game" :id="errorId('game')" class="text-xs text-danger">{{ errors.game }}</p>
    </fieldset>
    <p v-if="previousGameGone" class="rounded-panel border border-warn/40 bg-warn/10 p-3 text-sm text-warn">
      Your last server ran a game this service no longer hosts, so pick one of these.
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
            :min="limitOf(spec.field)?.min"
            :max="limitOf(spec.field)?.max"
            :disabled="busy"
            :aria-invalid="errors[spec.field] ? 'true' : undefined"
            :aria-describedby="describedBy(spec.field)"
            :class="[inputBase, 'w-28']"
          />
          <span v-if="spec.unit" class="text-sm text-ink-muted">{{ spec.unit }}</span>
        </div>
        <p :id="hintId(spec.field)" class="text-xs text-ink-muted">
          {{ numberHint(spec) }}
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

    <!-- /exp: shown when the game has an expansion to choose. -->
    <fieldset
      v-if="expansions.length > 1"
      class="min-w-0 space-y-2"
      :aria-describedby="describedBy('expansion')"
    >
      <legend
        :id="fieldId('expansion')"
        tabindex="-1"
        :class="[labelClass, 'focus:outline-none']"
      >
        {{ FIELD_LABELS.expansion }}
      </legend>
      <div class="flex flex-col gap-2 sm:flex-row sm:gap-6">
        <label v-for="option in expansions" :key="option.code" class="flex items-center gap-3 text-sm text-ink">
          <input
            v-model="form.expansion"
            type="radio"
            :name="`${baseId}-expansion`"
            :value="option.code"
            :disabled="busy"
            class="h-4 w-4 border-border bg-surface text-accent focus:ring-accent"
          />
          {{ option.name }}
        </label>
      </div>
      <p :id="hintId('expansion')" class="text-xs text-ink-muted">
        Players need the same expansion to join: a player without it installed cannot join a server that
        runs it.
      </p>
      <p v-if="errors.expansion" :id="errorId('expansion')" class="text-xs text-danger">{{ errors.expansion }}</p>
    </fieldset>

    <p
      v-if="droppedMissions.length > 0"
      class="rounded-panel border border-warn/40 bg-warn/10 p-3 text-sm text-warn"
    >
      Your last server's rotation had
      {{ droppedMissions.length === 1 ? 'a mission' : `${droppedMissions.length} missions` }} this game no
      longer offers, so {{ droppedMissions.length === 1 ? 'it was' : 'they were' }} left out:
      <span class="break-all font-mono text-xs">{{ droppedMissions.join(', ') }}</span>.
    </p>

    <MissionRotationPicker
      ref="rotation"
      v-model="form.missions"
      :available="dataSet?.default_missions ?? []"
      :base-id="`${baseId}-missions`"
      :error="errors.missions"
      :disabled="busy"
    />

    <!-- The own-machine add-on: shown while one of the account's own machines
         is free. -->
    <div v-if="ownMachinesFree > 0" class="min-w-0 space-y-1">
      <label class="flex items-center gap-3 text-sm text-ink">
        <input
          :id="`${baseId}-own_machine`"
          v-model="ownMachine"
          type="checkbox"
          :disabled="busy"
          :aria-describedby="`${baseId}-own_machine-hint`"
          class="h-4 w-4 rounded border-border bg-surface text-accent focus:ring-accent"
        />
        Own machine
      </label>
      <p :id="`${baseId}-own_machine-hint`" class="text-xs text-ink-muted">
        Run this server alone on one of your own machines instead of a shared one.
        {{ ownMachinesFree === 1 ? 'One of your own machines is free.' : `${ownMachinesFree} of your own machines are free.` }}
      </p>
    </div>

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
// A hosted server's settings and its start (POST /api/me/servers), on the
// account's start page (pages/account/NewServerPage.vue), offered only while
// its entitlement has room. Started from an ended server, it begins with that
// server's settings (after a failed one, the owner's try again). The
// own-machine checkbox shows while one of the account's own machines is free.
// The game (a data set the service offers) comes first and drives the rest:
// the expansions it offers, the whole-number ranges and the rotation's
// missions. Every field is checked against the backend's rule
// (utils/hostedSettings.ts) before the request; a refusal the backend gives
// on one field (invalid_settings) marks that field with the rule, never the
// backend's text.
import { computed, nextTick, reactive, ref, watch } from 'vue';
import MissionRotationPicker from './MissionRotationPicker.vue';
import { startHostedServer, type HostedError } from '../api/hostedServer';
import type {
  HostedDataSet,
  HostedExpansionOption,
  HostedServer,
  HostedSettings,
  SettingRange,
} from '../types/hostedServer';
import { cp1252Length } from '../utils/cp1252';
import {
  allowsExpansion,
  checkSettings,
  FIELD_LABELS,
  fieldRule,
  findDataSet,
  formFrom,
  GAME_NAME_MAX,
  GAME_PASSWORD_MAX,
  initialGame,
  initialRotation,
  type IntField,
  type SettingsErrors,
  type SettingsField,
} from '../utils/hostedSettings';

const props = withDefaults(
  defineProps<{
    // An ended server's settings, to start it again; null for none.
    previous: HostedSettings | null;
    // The games the service offers (GET /api/me/servers data_sets), in its order.
    dataSets: HostedDataSet[];
    // The server started from failed: the start is another try at it.
    retry?: boolean;
    // The base of every id in the form.
    baseId?: string;
    // How many of the account's own machines are free; the own-machine
    // checkbox shows while any is.
    ownMachinesFree?: number;
  }>(),
  { retry: false, baseId: 'hosted-settings', ownMachinesFree: 0 },
);

const emit = defineEmits<{
  // The service took the request (202); the server is `requested`.
  (e: 'started', server: HostedServer): void;
  // A refusal the page acts on besides showing it here (a session that ended,
  // the entitlement or hosting gone).
  (e: 'refused', error: HostedError): void;
}>();

// Run on one of the account's own machines.
const ownMachine = ref(false);
const labelClass = 'block text-xs font-medium uppercase tracking-wider text-ink-muted';
const inputBase =
  'rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none disabled:opacity-60 aria-[invalid=true]:border-danger';
const inputClass = `w-full ${inputBase}`;

// `noLimit`: the field's top value is the host screen's "no limit".
const NUMBER_FIELDS: { field: IntField; unit?: string; hint?: string; noLimit?: boolean }[] = [
  { field: 'max_players', unit: 'players' },
  { field: 'time_limit', unit: 'minutes' },
  { field: 'kill_limit', noLimit: true },
  { field: 'max_score', noLimit: true },
  { field: 'respawn', unit: 'seconds' },
  { field: 'start_delay', unit: 'seconds', hint: 'The wait before each round starts.' },
];

// The fields in the order the page shows them: the first refused one takes
// the keyboard.
const FIELD_ORDER: SettingsField[] = [
  'game',
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

// The game a fresh form picks (the last server's while still offered), and
// the rotation it begins with for each game: the last server's when it ran
// that game (each mission once, kept to the game's list), else the game's own.
const firstGame = initialGame(props.previous, props.dataSets);
const previousGameGone =
  props.previous !== null && props.dataSets.length > 0 && firstGame !== props.previous.game;
function rotationFor(game: string) {
  return initialRotation(props.previous, findDataSet(props.dataSets, game));
}

const form = reactive(
  formFrom(props.previous, findDataSet(props.dataSets, firstGame), rotationFor(firstGame).missions),
);
const dataSet = computed(() => findDataSet(props.dataSets, form.game));
const expansions = computed<HostedExpansionOption[]>(() => dataSet.value?.expansions ?? []);
// The last server's missions the picked game's rotation left out, said once
// above the picker.
const droppedMissions = computed(() => rotationFor(form.game).dropped);

// Another game: its expansions, ranges and missions. An expansion it lacks
// goes back to its base game, and the rotation starts again from its own.
watch(
  () => form.game,
  (game) => {
    const picked = findDataSet(props.dataSets, game);
    if (!picked || !allowsExpansion(picked, form.expansion)) form.expansion = '';
    form.missions = rotationFor(game).missions;
  },
);

function limitOf(field: IntField): SettingRange | null {
  return dataSet.value?.limits[field] ?? null;
}

function numberHint(spec: { field: IntField; hint?: string; noLimit?: boolean }): string {
  const range = limitOf(spec.field);
  const parts: string[] = [];
  if (range) parts.push(`${range.min} to ${range.max}.`);
  if (range && spec.noLimit) parts.push(`${range.max} means no limit.`);
  if (spec.hint) parts.push(spec.hint);
  return parts.join(' ');
}

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
    if (attempted.value) checked.value = checkSettings(form, props.dataSets).errors;
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
  return `${props.baseId}-${field}`;
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
  const result = checkSettings(form, props.dataSets);
  checked.value = result.errors;
  if (!result.settings) {
    const count = Object.keys(result.errors).length;
    alertText.value = `Check the ${count === 1 ? 'setting' : `${count} settings`} marked below.`;
    await focusFirstError(result.errors);
    return;
  }
  alertText.value = '';
  busy.value = true;
  const reply = await startHostedServer(result.settings, props.ownMachinesFree > 0 && ownMachine.value);
  busy.value = false;
  if (reply.ok) {
    emit('started', reply.value);
    return;
  }
  const error = reply.error;
  alertText.value = error.message;
  if (error.code === 'invalid_settings' && error.field) {
    const rule = fieldRule(error.field, dataSet.value);
    const refused: SettingsErrors = {};
    refused[error.field] = rule;
    serverRefusal.value = { field: error.field, message: rule };
    await focusFirstError(refused);
  }
  emit('refused', error);
}
</script>
