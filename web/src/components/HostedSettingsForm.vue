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
            : 'It stays listed until you stop it.'
        }}
      </p>
    </div>

    <!-- Live region, always in the form so a screen reader hears what lands in it. -->
    <p role="alert" class="sr-only">{{ alertText }}</p>
    <p
      v-if="alertText"
      :id="`${baseId}-alert`"
      class="rounded-panel border border-danger/40 bg-danger/10 p-3 text-sm text-danger"
    >
      <span aria-hidden="true">{{ alertText }}</span>
      <RouterLink v-if="alertLinksToTiers" to="/me/support" class="ml-1 text-accent hover:text-ink-bright">
        See your support tier
      </RouterLink>
    </p>

    <!-- The data set: the game the server runs, with the version of its data. -->
    <fieldset class="min-w-0 space-y-2" :aria-describedby="describedBy('data_set_id')">
      <legend :id="fieldId('data_set_id')" tabindex="-1" :class="[labelClass, 'focus:outline-none']">
        {{ FIELD_LABELS.data_set_id }}
      </legend>
      <div class="flex flex-col gap-2 sm:flex-row sm:flex-wrap sm:gap-6">
        <label v-for="option in dataSets" :key="option.id" class="flex items-center gap-3 text-sm text-ink">
          <input
            v-model="form.data_set_id"
            type="radio"
            :name="`${baseId}-data_set_id`"
            :value="option.id"
            :disabled="busy"
            class="h-4 w-4 border-border bg-surface text-accent focus:ring-accent"
          />
          {{ option.display_name }}
        </label>
      </div>
      <p :id="hintId('data_set_id')" class="text-xs text-ink-muted">{{ dataSet?.version }}</p>
      <p v-if="errors.data_set_id" :id="errorId('data_set_id')" class="text-xs text-danger">{{ errors.data_set_id }}</p>
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
          {{ cp1252Length(form.game_name.trim()) }} of {{ GAME_NAME_MAX }} characters. Accented letters work; emoji and
          double quotes do not.
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
          Empty for a public server. Up to {{ GAME_PASSWORD_MAX }} letters, digits and ASCII punctuation; no spaces or
          double quotes.
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
      v-if="expansions.length > 0"
      class="min-w-0 space-y-2"
      :aria-describedby="describedBy('expansion_id')"
    >
      <legend
        :id="fieldId('expansion_id')"
        tabindex="-1"
        :class="[labelClass, 'focus:outline-none']"
      >
        {{ FIELD_LABELS.expansion_id }}
      </legend>
      <div class="flex flex-col gap-2 sm:flex-row sm:gap-6">
        <label class="flex items-center gap-3 text-sm text-ink">
          <input
            v-model="form.expansion_id"
            type="radio"
            :name="`${baseId}-expansion_id`"
            :value="null"
            :disabled="busy"
            class="h-4 w-4 border-border bg-surface text-accent focus:ring-accent"
          />
          Base game
        </label>
        <label v-for="option in expansions" :key="option.id" class="flex items-center gap-3 text-sm text-ink">
          <input
            v-model="form.expansion_id"
            type="radio"
            :name="`${baseId}-expansion_id`"
            :value="option.id"
            :disabled="busy"
            class="h-4 w-4 border-border bg-surface text-accent focus:ring-accent"
          />
          {{ option.display_name }}
        </label>
      </div>
      <p :id="hintId('expansion_id')" class="text-xs text-ink-muted">{{ expansionHint }}</p>
      <p v-if="errors.expansion_id" :id="errorId('expansion_id')" class="text-xs text-danger">{{ errors.expansion_id }}</p>
    </fieldset>

    <p
      v-if="droppedMissions.length > 0"
      class="rounded-panel border border-warn/40 bg-warn/10 p-3 text-sm text-warn"
    >
      Your last server's rotation had
      {{ droppedMissions.length === 1 ? 'a mission' : `${droppedMissions.length} missions` }} this game's data no
      longer holds, so {{ droppedMissions.length === 1 ? 'it was' : 'they were' }} left out:
      <span class="break-all font-mono text-xs">{{ droppedMissions.join(', ') }}</span>.
    </p>

    <MissionRotationPicker
      ref="rotation"
      v-model="form.missions"
      :available="missions"
      :base-id="`${baseId}-missions`"
      :error="errors.missions"
      :disabled="busy"
    />

    <!-- Where it runs, while one of the account's dedicated machines is free:
         a choice while a server on shared machines is free too, else on the
         dedicated machine. -->
    <div v-if="ownMachinesFree > 0" class="min-w-0 space-y-1">
      <fieldset v-if="sharedFree > 0" :id="`${baseId}-pool`" class="space-y-2">
        <legend :class="labelClass">Where it runs</legend>
        <div class="flex flex-wrap gap-x-6 gap-y-2">
          <label class="flex items-center gap-2 text-sm text-ink">
            <input
              :id="`${baseId}-shared_machine`"
              v-model="ownMachine"
              type="radio"
              :name="`${baseId}-pool`"
              :value="false"
              :disabled="busy"
              :aria-describedby="`${baseId}-own_machine-hint`"
              class="h-4 w-4 border-border bg-surface text-accent focus:ring-accent"
            />
            Shared machine
          </label>
          <label class="flex items-center gap-2 text-sm text-ink">
            <input
              :id="`${baseId}-own_machine`"
              v-model="ownMachine"
              type="radio"
              :name="`${baseId}-pool`"
              :value="true"
              :disabled="busy"
              :aria-describedby="`${baseId}-own_machine-hint`"
              class="h-4 w-4 border-border bg-surface text-accent focus:ring-accent"
            />
            Dedicated machine
          </label>
        </div>
      </fieldset>
      <p v-else :id="`${baseId}-pool`" class="text-sm text-ink">On your dedicated machine</p>
      <p :id="`${baseId}-own_machine-hint`" class="text-xs text-ink-muted">
        {{ ownMachinesFree === 1 ? 'One of your dedicated machines is free.' : `${ownMachinesFree} of your dedicated machines are free.` }}
      </p>
      <!-- A static address for it: shown while one of the account's is free. -->
      <div v-if="onDedicated && addresses.length > 0" class="space-y-1 pt-2">
        <label :for="`${baseId}-address_id`" :class="labelClass">Static address</label>
        <select
          :id="`${baseId}-address_id`"
          v-model="addressId"
          :disabled="busy"
          :aria-describedby="`${baseId}-address_id-hint`"
          class="max-w-full rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none disabled:opacity-60"
        >
          <option :value="null">No static address</option>
          <option v-for="a in addresses" :key="a.id" :value="a.id">{{ a.hostname }}</option>
        </select>
        <p :id="`${baseId}-address_id-hint`" class="text-xs text-ink-muted">
          With one, players find the server at the same address and name every time it starts.
        </p>
      </div>
    </div>
    <!-- Every dedicated machine of the account's in use: it runs on a shared machine. -->
    <p v-else-if="ownMachines > 0" :id="`${baseId}-pool`" class="text-sm text-ink">
      On a shared machine: your dedicated machines are all in use.
    </p>

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
// server's settings (after a failed one, the owner's try again). While one of
// the account's dedicated machines is free it offers a choice of where the
// server runs (a shared machine or a dedicated one, the dedicated one from
// the start when the server started again ran on one), or with no server on
// shared machines free runs it on the dedicated machine; there, a pick of the
// account's free static addresses (the one `preferredAddressId` names first)
// while any is.
// The game (a base data set the service offers, by id) comes first and drives
// the rest: the expansions it offers (expansion data sets with data), the
// whole-number ranges and the rotation's missions, which are the game's data's
// own (and the picked expansion's), all of them unless the owner takes some
// out. Every field is checked against the backend's rule
// (utils/hostedSettings.ts) before the request; a refusal the backend gives
// on one field (invalid_settings) marks that field with the rule, never the
// backend's text.
import { computed, nextTick, reactive, ref, watch } from 'vue';
import MissionRotationPicker from './MissionRotationPicker.vue';
import { RouterLink } from 'vue-router';
import { linksToSupportTier, startHostedServer, type HostedError } from '../api/hostedServer';
import type { Address } from '../types/addresses';
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
  availableMissions,
  checkSettings,
  FIELD_LABELS,
  fieldRule,
  findDataSet,
  formFrom,
  GAME_NAME_MAX,
  GAME_PASSWORD_MAX,
  initialDataSet,
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
    // How many more servers the account may start on shared machines (1
    // unless said: the form was offered, so there is room), and how many of
    // its dedicated machines are free; the choice of where it runs shows
    // while both have room.
    sharedFree?: number;
    ownMachinesFree?: number;
    // The account's dedicated machines, free or not.
    ownMachines?: number;
    // The ended server started again ran on a dedicated machine: the choice
    // starts there (while one is free).
    previousOwnMachine?: boolean;
    // The account's static addresses a new server may run at (ready, and no
    // live server's), and the one the pick starts on.
    addresses?: Address[];
    preferredAddressId?: number | null;
  }>(),
  {
    retry: false,
    baseId: 'hosted-settings',
    sharedFree: 1,
    ownMachinesFree: 0,
    ownMachines: 0,
    previousOwnMachine: false,
    addresses: () => [],
    preferredAddressId: null,
  },
);

const emit = defineEmits<{
  // The service took the request (202); the server is `requested`.
  (e: 'started', server: HostedServer): void;
  // A refusal the page acts on besides showing it here (a session that ended,
  // the entitlement or hosting gone).
  (e: 'refused', error: HostedError): void;
}>();

// Run on one of the account's dedicated machines: the choice, which starts
// where the server started again ran; with no server on shared machines
// free, always (while one is free).
const ownMachine = ref(props.previousOwnMachine);
const onDedicated = computed(() => props.ownMachinesFree > 0 && (ownMachine.value || props.sharedFree === 0));
// The static address it runs at there, null for none: the preferred one while
// it is free, else the first free one.
function firstAddress(): number | null {
  const preferred = props.preferredAddressId;
  if (preferred !== null && props.addresses.some((a) => a.id === preferred)) return preferred;
  return props.addresses[0]?.id ?? null;
}
const addressId = ref<number | null>(firstAddress());
// The page read the addresses again: a pick taken meanwhile starts over.
watch(
  () => props.addresses,
  (list) => {
    if (addressId.value !== null && !list.some((a) => a.id === addressId.value)) addressId.value = firstAddress();
  },
);
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
  'data_set_id',
  'game_name',
  'game_password',
  'max_players',
  'time_limit',
  'kill_limit',
  'max_score',
  'respawn',
  'start_delay',
  'team_ff',
  'expansion_id',
  'missions',
];

// The game a fresh form picks (the last server's while still offered), and
// the rotation it begins with for each game and expansion: the last server's
// when it ran that game (each mission once, kept to the data's list), else
// every mission the data holds.
const firstDataSet = initialDataSet(props.previous, props.dataSets);
const previousGameGone =
  props.previous !== null && props.dataSets.length > 0 && firstDataSet !== props.previous.data_set_id;
function rotationFor(dataSetId: number, expansionId: number | null) {
  return initialRotation(props.previous, findDataSet(props.dataSets, dataSetId), expansionId);
}

const firstForm = formFrom(props.previous, findDataSet(props.dataSets, firstDataSet), []);
firstForm.missions = rotationFor(firstDataSet, firstForm.expansion_id).missions;
const form = reactive(firstForm);
const dataSet = computed(() => findDataSet(props.dataSets, form.data_set_id));
const expansions = computed<HostedExpansionOption[]>(() => dataSet.value?.expansions ?? []);
const missions = computed(() => availableMissions(dataSet.value, form.expansion_id));
// The picked expansion's version; players need it installed to join.
const expansionHint = computed(() => {
  const picked = expansions.value.find((e) => e.id === form.expansion_id);
  if (!picked) return 'Players need an expansion installed to join a server running it.';
  return picked.version ? `${picked.version}. Players need it installed to join.` : 'Players need it installed to join.';
});
// The last server's missions the picked data's list left out, said once
// above the picker.
const droppedMissions = computed(() => rotationFor(form.data_set_id, form.expansion_id).dropped);

// Another game: its expansions, ranges and missions. An expansion it lacks
// goes back to its base game, and the rotation starts again from the data's.
watch(
  () => form.data_set_id,
  (id) => {
    const picked = findDataSet(props.dataSets, id);
    if (!picked || !allowsExpansion(picked, form.expansion_id)) form.expansion_id = null;
    form.missions = rotationFor(id, form.expansion_id).missions;
  },
);

// Another expansion: its missions join (or leave) the rotation's list, so
// the rotation starts again from the data's.
watch(
  () => form.expansion_id,
  (expansionId) => {
    form.missions = rotationFor(form.data_set_id, expansionId).missions;
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
// The refusal shown is one a support tier answers: the alert links to it.
const alertLinksToTiers = ref(false);
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
  alertLinksToTiers.value = false;
  if (!result.settings) {
    const count = Object.keys(result.errors).length;
    alertText.value = `Check the ${count === 1 ? 'setting' : `${count} settings`} marked below.`;
    await focusFirstError(result.errors);
    return;
  }
  alertText.value = '';
  busy.value = true;
  const own = onDedicated.value;
  const reply = await startHostedServer(result.settings, own, own && props.addresses.length > 0 ? addressId.value : null);
  busy.value = false;
  if (reply.ok) {
    emit('started', reply.value);
    return;
  }
  const error = reply.error;
  alertText.value = error.message;
  alertLinksToTiers.value = linksToSupportTier(error.code);
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
