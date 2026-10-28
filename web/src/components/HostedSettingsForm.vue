<template>
  <form class="card space-y-6 p-4 sm:p-6" novalidate :aria-label="retry ? 'Start your server again' : 'New server'" @submit.prevent="submit">
    <!-- Live region, always in the form so a screen reader hears what lands in it. -->
    <p role="alert" class="sr-only">{{ alertText }}</p>
    <Notice v-if="alertText" :id="`${baseId}-alert`" tone="bad">
      <span aria-hidden="true">{{ alertText }}</span>
      <RouterLink v-if="alertLinksToTiers" to="/me/billing" class="link ml-1">Billing</RouterLink>
    </Notice>

    <!-- The game the server runs, with the version of its data. -->
    <fieldset class="min-w-0 space-y-2" :aria-describedby="describedBy('product_id')">
      <legend :id="fieldId('product_id')" tabindex="-1" class="field-label focus:outline-none">{{ FIELD_LABELS.product_id }}</legend>
      <div class="flex flex-col gap-2 sm:flex-row sm:flex-wrap sm:gap-6">
        <label v-for="option in products" :key="option.id" class="flex items-center gap-3 text-sm text-ink">
          <input
            v-model="form.product_id"
            type="radio"
            :name="`${baseId}-product_id`"
            :value="option.id"
            :disabled="busy"
            class="h-4 w-4 border-border bg-surface text-accent focus:ring-accent"
          />
          {{ option.display_name }}
        </label>
      </div>
      <p :id="hintId('product_id')" class="text-xs text-ink-muted">{{ product?.version }}</p>
      <p v-if="errors.product_id" :id="errorId('product_id')" class="text-xs text-danger">{{ errors.product_id }}</p>
    </fieldset>
    <Notice v-if="previousGameGone" tone="warn">Your last server's game is no longer hosted: pick another.</Notice>

    <div class="grid gap-4 sm:grid-cols-2">
      <!-- GameName -->
      <div class="min-w-0 space-y-1">
        <label :for="fieldId('game_name')" class="field-label">{{ FIELD_LABELS.game_name }}</label>
        <input
          :id="fieldId('game_name')"
          v-model="form.game_name"
          type="text"
          class="field"
          autocomplete="off"
          :disabled="busy"
          :maxlength="GAME_NAME_MAX * 2"
          :aria-invalid="errors.game_name ? 'true' : undefined"
          :aria-describedby="describedBy('game_name')"
        />
        <p :id="hintId('game_name')" class="text-xs text-ink-muted">{{ cp1252Length(form.game_name.trim()) }}/{{ GAME_NAME_MAX }}</p>
        <p v-if="errors.game_name" :id="errorId('game_name')" class="text-xs text-danger">{{ errors.game_name }}</p>
      </div>

      <!-- MPHostGamePassword -->
      <div class="min-w-0 space-y-1">
        <label :for="fieldId('game_password')" class="field-label">{{ FIELD_LABELS.game_password }} (optional)</label>
        <div class="flex gap-2">
          <input
            :id="fieldId('game_password')"
            v-model="form.game_password"
            :type="showPassword ? 'text' : 'password'"
            class="field min-w-0 flex-1 font-mono"
            placeholder="None: a public server"
            :title="`Up to ${GAME_PASSWORD_MAX} letters, digits and punctuation`"
            autocomplete="off"
            autocapitalize="none"
            spellcheck="false"
            :disabled="busy"
            :maxlength="GAME_PASSWORD_MAX * 2"
            :aria-invalid="errors.game_password ? 'true' : undefined"
            :aria-describedby="describedBy('game_password', false)"
          />
          <button
            type="button"
            class="btn-secondary btn-sm"
            :aria-pressed="showPassword ? 'true' : 'false'"
            :aria-controls="fieldId('game_password')"
            @click="showPassword = !showPassword"
          >
            {{ showPassword ? 'Hide' : 'Show' }}<span class="sr-only"> the game password</span>
          </button>
        </div>
        <p v-if="errors.game_password" :id="errorId('game_password')" class="text-xs text-danger">{{ errors.game_password }}</p>
      </div>

      <!-- MaxPlayers -->
      <div v-for="spec in NUMBER_FIELDS.slice(0, 1)" :key="spec.field" class="min-w-0 space-y-1">
        <label :for="fieldId(spec.field)" class="field-label">{{ FIELD_LABELS[spec.field] }}</label>
        <div class="flex items-center gap-2">
          <input
            :id="fieldId(spec.field)"
            v-model.number="form[spec.field]"
            type="number"
            class="field w-28"
            inputmode="numeric"
            step="1"
            :min="limitOf(spec.field)?.min"
            :max="limitOf(spec.field)?.max"
            :disabled="busy"
            :aria-invalid="errors[spec.field] ? 'true' : undefined"
            :aria-describedby="describedBy(spec.field)"
          />
          <span v-if="spec.unit" class="text-sm text-ink-muted">{{ spec.unit }}</span>
        </div>
        <p :id="hintId(spec.field)" class="text-xs text-ink-muted">{{ numberHint(spec) }}</p>
        <p v-if="errors[spec.field]" :id="errorId(spec.field)" class="text-xs text-danger">{{ errors[spec.field] }}</p>
      </div>

      <!-- Listed on NovaLogic's NovaWorld too: shown while the service offers it and the game takes it. -->
      <div v-if="showNovaLogic" class="min-w-0 space-y-1 self-center">
        <label class="flex items-center gap-3 text-sm text-ink">
          <input
            :id="fieldId('list_on_novalogic')"
            v-model="form.list_on_novalogic"
            type="checkbox"
            :disabled="busy"
            :aria-invalid="errors.list_on_novalogic ? 'true' : undefined"
            :aria-describedby="describedBy('list_on_novalogic', false)"
            class="h-4 w-4 rounded border-border bg-surface text-accent focus:ring-accent"
          />
          {{ FIELD_LABELS.list_on_novalogic }}
        </label>
        <p v-if="errors.list_on_novalogic" :id="errorId('list_on_novalogic')" class="text-xs text-danger">
          {{ errors.list_on_novalogic }}
        </p>
      </div>
    </div>

    <!-- /exp: shown when the game has an expansion to choose. -->
    <fieldset v-if="expansions.length > 0" class="min-w-0 space-y-2" :aria-describedby="describedBy('expansion_id', expansionHint !== '')">
      <legend :id="fieldId('expansion_id')" tabindex="-1" class="field-label focus:outline-none">{{ FIELD_LABELS.expansion_id }}</legend>
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
      <p v-if="expansionHint" :id="hintId('expansion_id')" class="text-xs text-ink-muted">{{ expansionHint }}</p>
      <p v-if="errors.expansion_id" :id="errorId('expansion_id')" class="text-xs text-danger">{{ errors.expansion_id }}</p>
    </fieldset>

    <Notice v-if="droppedMissions.length > 0" tone="warn">
      Left out, no longer in this game's data: <span class="break-all font-mono text-xs">{{ droppedMissions.join(', ') }}</span>
    </Notice>

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
        <legend class="field-label">Where it runs</legend>
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
        <label :for="`${baseId}-address_id`" class="field-label" title="The same address and name every time it starts">Static address</label>
        <select :id="`${baseId}-address_id`" v-model="addressId" class="field w-auto max-w-full" :disabled="busy">
          <option :value="null">No static address</option>
          <option v-for="a in addresses" :key="a.id" :value="a.id">{{ a.hostname }}</option>
        </select>
      </div>
    </div>
    <!-- Every dedicated machine of the account's in use: it runs on a shared machine. -->
    <p v-else-if="ownMachines > 0" :id="`${baseId}-pool`" class="text-sm text-ink">On a shared machine: your dedicated machines are all in use.</p>

    <details class="rounded-panel border border-border bg-surface p-4" data-advanced-settings>
      <summary class="font-medium text-ink">Advanced settings</summary>
      <div class="mt-4 grid gap-4 sm:grid-cols-2">
        <!-- Time_Limit, KillLimit, MaxScore, Respawn, Delay -->
        <div v-for="spec in NUMBER_FIELDS.slice(1)" :key="spec.field" class="min-w-0 space-y-1">
          <label :for="fieldId(spec.field)" class="field-label" :title="spec.hint">{{ FIELD_LABELS[spec.field] }}</label>
          <div class="flex items-center gap-2">
            <input
              :id="fieldId(spec.field)"
              v-model.number="form[spec.field]"
              type="number"
              class="field w-28"
              inputmode="numeric"
              step="1"
              :min="limitOf(spec.field)?.min"
              :max="limitOf(spec.field)?.max"
              :disabled="busy"
              :aria-invalid="errors[spec.field] ? 'true' : undefined"
              :aria-describedby="describedBy(spec.field)"
            />
            <span v-if="spec.unit" class="text-sm text-ink-muted">{{ spec.unit }}</span>
          </div>
          <p :id="hintId(spec.field)" class="text-xs text-ink-muted">{{ numberHint(spec) }}</p>
          <p v-if="errors[spec.field]" :id="errorId(spec.field)" class="text-xs text-danger">{{ errors[spec.field] }}</p>
        </div>

        <!-- TeamFF -->
        <label class="flex items-center gap-3 text-sm text-ink sm:col-span-2">
          <input
            :id="fieldId('team_ff')"
            v-model="form.team_ff"
            type="checkbox"
            :disabled="busy"
            class="h-4 w-4 rounded border-border bg-surface text-accent focus:ring-accent"
          />
          {{ FIELD_LABELS.team_ff }}
        </label>
      </div>
    </details>

    <div class="flex justify-end border-t border-border pt-4">
      <button type="submit" class="btn-primary" :disabled="busy">{{ busy ? 'Starting…' : retry ? 'Try again' : 'Start server' }}</button>
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
// while any is. While the service offers it, a box lists the server on
// NovaLogic's NovaWorld too; else the start sends it off.
// The game (a base game the service offers, by its product id) comes first and drives
// the rest: the expansions it offers (expansion games with data), the
// whole-number ranges and the rotation's missions, which are the game's data's
// own (and the picked expansion's), all of them unless the owner takes some
// out. Every field is checked against the backend's rule
// (utils/hostedSettings.ts) before the request; a refusal the backend gives
// on one field (invalid_settings) marks that field with the rule, never the
// backend's text.
import { computed, nextTick, reactive, ref, watch } from 'vue';
import { RouterLink } from 'vue-router';
import MissionRotationPicker from './MissionRotationPicker.vue';
import Notice from './ui/Notice.vue';
import { linksToSupportTier, startHostedServer, type HostedError } from '../api/hostedServer';
import type { Address } from '../types/addresses';
import type {
  HostedExpansionOption,
  HostedProduct,
  HostedServer,
  HostedSettings,
  SettingRange,
} from '../types/hostedServer';
import { cp1252Length } from '../utils/cp1252';
import { useUnsavedChanges } from '../composables/useUnsavedChanges';
import {
  allowsExpansion,
  availableMissions,
  checkSettings,
  FIELD_LABELS,
  fieldRule,
  findProduct,
  formFrom,
  GAME_NAME_MAX,
  GAME_PASSWORD_MAX,
  initialProduct,
  initialRotation,
  offersNovaLogicListing,
  type IntField,
  type SettingsErrors,
  type SettingsField,
} from '../utils/hostedSettings';

const props = withDefaults(
  defineProps<{
    // An ended server's settings, to start it again; null for none.
    previous: HostedSettings | null;
    // The games the service offers (GET /api/me/servers products), in its order.
    products: HostedProduct[];
    // The service offers listing the server on NovaLogic's NovaWorld too (GET
    // /api/me/servers novalogic_listing): the box shows, and its value goes.
    novalogicListing?: boolean;
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
    novalogicListing: false,
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
// `noLimit`: the field's top value is the host screen's "no limit"; `hint`
// shows as the label's tooltip.
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
  'product_id',
  'game_name',
  'game_password',
  'max_players',
  'list_on_novalogic',
  'expansion_id',
  'missions',
  'time_limit',
  'kill_limit',
  'max_score',
  'respawn',
  'start_delay',
  'team_ff',
];

// The game a fresh form picks (the last server's while still offered), and
// the rotation it begins with for each game and expansion: the last server's
// when it ran that game (each mission once, kept to the data's list), else
// every mission the data holds.
const firstProduct = initialProduct(props.previous, props.products);
const previousGameGone =
  props.previous !== null && props.products.length > 0 && firstProduct !== props.previous.product_id;
function rotationFor(productId: number, expansionId: number | null) {
  return initialRotation(props.previous, findProduct(props.products, productId), expansionId);
}

const firstForm = formFrom(props.previous, findProduct(props.products, firstProduct), []);
firstForm.missions = rotationFor(firstProduct, firstForm.expansion_id).missions;
const form = reactive(firstForm);
const submitted = ref(false);
const settingsSnapshot = () => JSON.stringify({ settings: form, ownMachine: ownMachine.value, addressId: addressId.value });
const initialSettings = settingsSnapshot();
useUnsavedChanges(() => !submitted.value && settingsSnapshot() !== initialSettings);
const product = computed(() => findProduct(props.products, form.product_id));
// The NovaLogic box: the service offers it and the picked game takes it (Joint Operations alone).
const showNovaLogic = computed(() => offersNovaLogicListing(product.value, props.novalogicListing));
const expansions = computed<HostedExpansionOption[]>(() => product.value?.expansions ?? []);
const missions = computed(() => availableMissions(product.value, form.expansion_id));
// The picked expansion's version; players need it installed to join.
const expansionHint = computed(() => {
  const picked = expansions.value.find((e) => e.id === form.expansion_id);
  if (!picked) return '';
  return picked.version ? `${picked.version}, needed to join` : 'Needed to join';
});
// The last server's missions the picked data's list left out, said once
// above the picker.
const droppedMissions = computed(() => rotationFor(form.product_id, form.expansion_id).dropped);

// Another game: its expansions, ranges and missions. An expansion it lacks
// goes back to its base game, and the rotation starts again from the data's.
watch(
  () => form.product_id,
  (id) => {
    const picked = findProduct(props.products, id);
    if (!picked || !allowsExpansion(picked, form.expansion_id)) form.expansion_id = null;
    form.missions = rotationFor(id, form.expansion_id).missions;
  },
);

// Another expansion: its missions join (or leave) the rotation's list, so
// the rotation starts again from the data's.
watch(
  () => form.expansion_id,
  (expansionId) => {
    form.missions = rotationFor(form.product_id, expansionId).missions;
  },
);

function limitOf(field: IntField): SettingRange | null {
  return product.value?.limits[field] ?? null;
}

function numberHint(spec: { field: IntField; noLimit?: boolean }): string {
  const range = limitOf(spec.field);
  const parts: string[] = [];
  if (range) parts.push(`${range.min} to ${range.max}.`);
  if (range && spec.noLimit) parts.push(`${range.max} means no limit.`);
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
    if (attempted.value) checked.value = checkSettings(form, props.products, props.novalogicListing).errors;
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
// A field's hint (unless it shows none) and its error while it has one.
function describedBy(field: SettingsField, hasHint = true): string | undefined {
  const ids = [...(hasHint ? [hintId(field)] : []), ...(errors.value[field] ? [errorId(field)] : [])];
  return ids.length > 0 ? ids.join(' ') : undefined;
}

async function focusFirstError(errs: SettingsErrors) {
  await nextTick();
  const first = FIELD_ORDER.find((f) => errs[f]);
  if (!first) return;
  if (first === 'missions') {
    rotation.value?.focus();
    return;
  }
  const field = document.getElementById(fieldId(first));
  const advanced = field?.closest('details');
  if (advanced) advanced.open = true;
  field?.focus();
}

async function submit() {
  if (busy.value) return;
  attempted.value = true;
  serverRefusal.value = null;
  const result = checkSettings(form, props.products, props.novalogicListing);
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
    submitted.value = true;
    emit('started', reply.value);
    return;
  }
  const error = reply.error;
  alertText.value = error.message;
  alertLinksToTiers.value = linksToSupportTier(error.code);
  if (error.code === 'invalid_settings' && error.field) {
    const rule = fieldRule(error.field, product.value);
    const refused: SettingsErrors = {};
    refused[error.field] = rule;
    serverRefusal.value = { field: error.field, message: rule };
    await focusFirstError(refused);
  }
  emit('refused', error);
}
</script>
