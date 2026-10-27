<template>
  <div class="space-y-8">
    <AdminPageHeader
      title="Settings"
      description="Whether support tiers sell, and how big the fleet may grow. Changes take effect at once, with no redeploy."
      :loaded-at="loadedAt"
      :now="now"
      :loading="loading"
      @refresh="load"
    />

    <div role="alert" :class="error ? 'rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger' : 'sr-only'">{{ error }}</div>
    <div role="status" :class="saved ? 'rounded-panel border border-border bg-panel p-4 text-sm text-ink' : 'sr-only'">{{ saved }}</div>
    <div v-if="settings === null && !error" class="animate-pulse rounded-panel border border-border bg-panel p-8 text-center text-ink-muted">
      Loading the settings…
    </div>

    <form v-if="settings && form" class="space-y-6" @submit.prevent="save">
      <section class="space-y-3 rounded-panel border border-border bg-panel p-5" aria-labelledby="settings-subscriptions">
        <h2 id="settings-subscriptions" class="text-lg font-semibold text-ink">Subscriptions</h2>
        <label class="flex items-center gap-3 text-sm text-ink">
          <input
            id="settings-subscriptions-enabled"
            v-model="form.subscriptions_enabled"
            type="checkbox"
            class="h-4 w-4 rounded border-border bg-surface text-accent focus:ring-accent"
            aria-describedby="settings-subscriptions-hint"
          />
          Subscriptions Enabled
        </label>
        <p id="settings-subscriptions-hint" class="text-xs text-ink-muted">
          Every account has one hosted server on shared machines, free of charge. On, players can also buy support tiers,
          dedicated machines and static addresses, each on top of the free server and any admin grant. Off, nothing sells;
          what an account already pays for keeps running and is managed in the billing portal.
        </p>
      </section>

      <section class="space-y-4 rounded-panel border border-border bg-panel p-5" aria-labelledby="settings-fleet">
        <h2 id="settings-fleet" class="text-lg font-semibold text-ink">Fleet</h2>
        <div class="flex flex-wrap items-end gap-6">
          <div v-for="field in fields" :key="field.key" class="space-y-1">
            <label :for="`settings-${field.key}`" :class="labelClass">{{ field.label }}</label>
            <input
              :id="`settings-${field.key}`"
              v-model.number="form[field.key]"
              type="number"
              :min="field.min"
              :max="field.max"
              step="1"
              inputmode="numeric"
              :disabled="field.key === 'max_addresses' && !settings.addresses_available"
              :aria-invalid="problemOf(field) ? 'true' : undefined"
              :aria-describedby="`settings-${field.key}-hint`"
              :class="numberClass"
            />
            <p :id="`settings-${field.key}-hint`" class="text-xs" :class="problemOf(field) ? 'text-danger' : 'text-ink-muted'">
              {{ problemOf(field) || `Deployment default ${settings.boot[field.key]}` }}
            </p>
          </div>
        </div>
        <p class="text-xs text-ink-muted">
          A new shared machine gets the slots set here; one already running keeps its own. Lowering a maximum ends no
          machine: the fleet launches no more past it, and a server waits for a slot while every machine is full.
          {{ settings.addresses_available ? '' : 'This deployment runs no static addresses.' }}
        </p>
        <p v-if="settings.capacity" id="settings-capacity" class="text-sm" :class="settings.capacity.oversold ? 'text-warn' : 'text-ink'">
          {{ soldCapacityText(settings.capacity) }}
          {{ settings.capacity.oversold ? 'Past what the fleet holds: nothing more of that sells until you raise it.' : '' }}
        </p>
      </section>

      <button type="submit" :disabled="busy || !changed || hasProblem" :class="primaryButton">Save settings</button>
    </form>
  </div>
</template>

<script setup lang="ts">
// /admin/settings: the admin's live settings (GET and PUT
// /api/admin/settings): Subscriptions Enabled, and the fleet's maximum shared
// machines, their slots, maximum dedicated machines and static addresses.
import { computed, onMounted, ref } from 'vue';
import AdminPageHeader from '../../components/AdminPageHeader.vue';
import { fetchSettings, saveSettings } from '../../api/settings';
import {
  FLEET_MACHINES_MAX,
  SHARED_SLOTS_MAX,
  type FleetLimits,
  type ServiceSettings,
  type ServiceSettingsChange,
} from '../../types/settings';
import { soldCapacityText } from '../../utils/adminView';

interface Field {
  key: keyof FleetLimits;
  label: string;
  min: number;
  max: number;
}

const fields: Field[] = [
  { key: 'max_shared', label: 'Max shared machines', min: 0, max: FLEET_MACHINES_MAX },
  { key: 'shared_slots', label: 'Slots per shared machine', min: 1, max: SHARED_SLOTS_MAX },
  { key: 'max_own', label: 'Max dedicated machines', min: 0, max: FLEET_MACHINES_MAX },
  { key: 'max_addresses', label: 'Max static addresses', min: 0, max: FLEET_MACHINES_MAX },
];

const labelClass = 'block text-xs font-medium uppercase tracking-wider text-ink-muted';
const numberClass =
  'w-28 rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none disabled:opacity-50 aria-[invalid=true]:border-danger';
const primaryButton =
  'rounded-control bg-accent px-4 py-2 text-sm font-medium text-on-accent transition hover:bg-accent/90 disabled:cursor-not-allowed disabled:opacity-50';

const settings = ref<ServiceSettings | null>(null);
const form = ref<ServiceSettingsChange | null>(null);
const error = ref('');
const saved = ref('');
const loading = ref(false);
const busy = ref(false);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());

function formOf(s: ServiceSettings): ServiceSettingsChange {
  return { subscriptions_enabled: s.subscriptions_enabled, ...s.fleet };
}

function problemOf(field: Field): string {
  const value = form.value?.[field.key];
  if (typeof value !== 'number' || !Number.isInteger(value) || value < field.min || value > field.max) {
    return `A whole number from ${field.min} to ${field.max}`;
  }
  return '';
}

const hasProblem = computed(() => fields.some((f) => problemOf(f) !== ''));
const changed = computed(() => {
  if (!settings.value || !form.value) return false;
  const before = formOf(settings.value);
  return (Object.keys(before) as (keyof ServiceSettingsChange)[]).some((k) => before[k] !== form.value![k]);
});

function show(s: ServiceSettings) {
  settings.value = s;
  form.value = formOf(s);
  now.value = Date.now();
  loadedAt.value = now.value;
}

async function load() {
  loading.value = true;
  const result = await fetchSettings();
  loading.value = false;
  if (result.ok) {
    show(result.value);
    error.value = '';
  } else {
    error.value = result.error.message;
  }
}

async function save() {
  if (!form.value || busy.value || hasProblem.value) return;
  busy.value = true;
  saved.value = '';
  const result = await saveSettings({ ...form.value });
  busy.value = false;
  if (!result.ok) {
    error.value = result.error.message;
    return;
  }
  error.value = '';
  show(result.value);
  saved.value = 'Settings saved.';
}

onMounted(load);
</script>
