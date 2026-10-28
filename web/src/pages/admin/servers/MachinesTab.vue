<template>
  <div class="space-y-6">
    <PageHeader title="Servers" :tabs="SERVERS_TABS" refreshable :loaded-at="loadedAt" :now="now" :loading="poll.loading.value" @refresh="poll.refresh" />

    <LiveNotices :message="message" :error="error" :poll-trouble="pollTrouble" />

    <FleetClusterPanel v-if="cluster || clusterUnread" :cluster="cluster" :unread="clusterUnread" />

    <details class="capacity card p-4">
      <summary class="cursor-pointer">
        <h2 class="section-heading inline">Capacity</h2>
        <span v-if="settings?.capacity" class="capacity-sold ml-3 text-sm" :class="settings.capacity.oversold ? 'text-warn' : 'text-ink-muted'">
          {{ soldCapacityText(settings.capacity) }}
        </span>
      </summary>
      <p v-if="settingsError" role="alert" class="mt-3 text-sm text-danger">{{ settingsError }}</p>
      <p v-else-if="!form" class="mt-3 text-sm text-ink-muted">Loading…</p>
      <form v-if="settings && form" class="mt-3 flex flex-wrap items-end gap-4" @submit.prevent="saveCapacity">
        <div v-for="field in FIELDS" :key="field.key">
          <label :for="`capacity-${field.key}`" class="field-label">{{ field.label }}</label>
          <input
            :id="`capacity-${field.key}`"
            v-model.number="form[field.key]"
            type="number"
            :min="field.min"
            :max="field.max"
            step="1"
            inputmode="numeric"
            :disabled="field.key === 'max_addresses' && !settings.addresses_available"
            :aria-invalid="problemOf(field) ? 'true' : undefined"
            :title="problemOf(field) || `Deployment default ${settings.boot[field.key]}`"
            class="field w-28"
          />
        </div>
        <button type="submit" class="btn-primary" :disabled="busy || !changed || hasProblem">Save</button>
      </form>
    </details>

    <section class="space-y-3" aria-labelledby="machines-heading">
      <div class="flex flex-wrap items-center gap-3">
        <h2 id="machines-heading" class="section-heading">Machines</h2>
        <HistoryToggle v-model="showHistory" :count="replacedIds.size" />
      </div>
      <p v-if="fleet === null" class="text-sm text-ink-muted">{{ poll.loading.value ? 'Loading…' : 'Not loaded.' }}</p>
      <p v-else-if="fleet.machines.length === 0" class="text-sm text-ink-muted">No machines.</p>
      <div v-else class="grid gap-4 lg:grid-cols-2 2xl:grid-cols-3">
        <MachineCard
          v-for="m in machinesShown"
          :key="m.id"
          :machine="m"
          :servers="fleet.servers"
          :now="now"
          :busy="busy"
          @drain="onMachine(m, 'drain')"
          @end="onMachine(m, 'end')"
        />
      </div>
    </section>
  </div>
</template>

<script setup lang="ts">
// /admin/servers/machines: the fleet's machines. ECS's view of the fleet
// cluster (FleetClusterPanel), the fleet's maximums (GET and PUT
// /api/admin/settings) behind Capacity, and a card per machine whose slots
// name the servers in them, with drain and end. Polls, faster while a
// machine changes state.
import { computed, onMounted, ref } from 'vue';
import FleetClusterPanel from '../../../components/FleetClusterPanel.vue';
import HistoryToggle from '../../../components/HistoryToggle.vue';
import MachineCard from '../../../components/MachineCard.vue';
import LiveNotices from '../../../components/ui/LiveNotices.vue';
import PageHeader from '../../../components/ui/PageHeader.vue';
import { fetchHostedFleet, machineAction } from '../../../api/admin';
import { fetchInfrastructure } from '../../../api/infra';
import { fetchSettings, saveSettings } from '../../../api/settings';
import { confirmAction } from '../../../composables/useConfirm';
import { usePolling } from '../../../composables/usePolling';
import type { FleetMachine, FleetResponse } from '../../../types/hostedServer';
import type { InfraFleetCluster } from '../../../types/infra';
import { FLEET_MACHINES_MAX, SHARED_SLOTS_MAX, type FleetLimits, type ServiceSettings } from '../../../types/settings';
import { SERVERS_TABS } from '../../../utils/adminNav';
import { soldCapacityText } from '../../../utils/adminView';
import { replacedMachines } from '../../../utils/history';
import { isTransitioning } from '../../../utils/hostedState';

const TRANSITION_POLL_MS = 5_000;
const IDLE_POLL_MS = 30_000;

interface Field {
  key: keyof FleetLimits;
  label: string;
  min: number;
  max: number;
}

const FIELDS: Field[] = [
  { key: 'max_shared', label: 'Max shared machines', min: 0, max: FLEET_MACHINES_MAX },
  { key: 'shared_slots', label: 'Slots per shared machine', min: 1, max: SHARED_SLOTS_MAX },
  { key: 'max_own', label: 'Max dedicated machines', min: 0, max: FLEET_MACHINES_MAX },
  { key: 'max_addresses', label: 'Max static addresses', min: 0, max: FLEET_MACHINES_MAX },
];

const fleet = ref<FleetResponse | null>(null);
const busy = ref(false);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const message = ref('');
const error = ref('');
const pollTrouble = ref(false);
const showHistory = ref(false);
let errorFromLoad = false;

const replacedIds = computed(() => replacedMachines(fleet.value?.machines ?? []));
const machinesShown = computed(() => (fleet.value?.machines ?? []).filter((m) => showHistory.value || !replacedIds.value.has(m.id)));

function flash(text: string) {
  message.value = text;
  error.value = '';
}

function fail(text: string) {
  error.value = text;
  message.value = '';
  errorFromLoad = false;
}

// The fleet cluster as ECS sees it: its failure leaves the rest of the page as
// it is.
const cluster = ref<InfraFleetCluster | null>(null);
const clusterUnread = ref('');
async function readCluster(signal: AbortSignal) {
  const result = await fetchInfrastructure(signal);
  if (signal.aborted) return;
  if (!result.ok) {
    if (!cluster.value) clusterUnread.value = result.error.message;
    return;
  }
  const cloud = result.value.cloud;
  if (!cloud.configured) {
    cluster.value = null;
    clusterUnread.value = cloud.unavailable;
  } else if (cloud.error) {
    clusterUnread.value = cloud.error;
  } else {
    cluster.value = cloud.fleet;
    clusterUnread.value = '';
  }
}

const poll = usePolling({
  async read(signal, background) {
    const [result] = await Promise.all([fetchHostedFleet({ state: 'live' }, signal), readCluster(signal)]);
    if (signal.aborted) return false;
    now.value = Date.now();
    if (result.ok) {
      fleet.value = result.value;
      loadedAt.value = now.value;
      pollTrouble.value = false;
      if (errorFromLoad) {
        error.value = '';
        errorFromLoad = false;
      }
      return true;
    }
    if (background && fleet.value !== null) {
      pollTrouble.value = true;
    } else {
      fail(result.error.message);
      errorFromLoad = true;
    }
    return false;
  },
  next: () => {
    const moving =
      (fleet.value?.servers.some((s) => isTransitioning(s.state)) ?? false) ||
      (fleet.value?.machines.some((m) => m.state !== 'ready' && m.state !== 'ended' && m.state !== 'failed') ?? false);
    return moving ? TRANSITION_POLL_MS : IDLE_POLL_MS;
  },
});

async function onMachine(m: FleetMachine, action: 'drain' | 'end') {
  if (busy.value) return;
  const n = m.slots_used;
  const servers = `${n} server${n === 1 ? '' : 's'}`;
  const prompt =
    action === 'drain'
      ? `Drain machine ${m.id}? Its ${servers} move to other machines one at a time.`
      : n === 0
        ? `End machine ${m.id}? No server runs on it.`
        : `End machine ${m.id} now? Its ${servers} move at once and their matches end.`;
  if (!(await confirmAction(prompt))) return;
  busy.value = true;
  const result = await machineAction(m.id, action);
  busy.value = false;
  if (!result.ok) fail(result.error.message);
  else flash(`Machine ${m.id} ${action === 'drain' ? 'draining' : 'ending'}.`);
  await poll.readAgain();
}

// The fleet's maximums, live (the subscriptions switch rides along unchanged).
const settings = ref<ServiceSettings | null>(null);
const form = ref<FleetLimits | null>(null);
const settingsError = ref('');

function problemOf(field: Field): string {
  const value = form.value?.[field.key];
  if (typeof value !== 'number' || !Number.isInteger(value) || value < field.min || value > field.max) {
    return `A whole number from ${field.min} to ${field.max}`;
  }
  return '';
}
const hasProblem = computed(() => FIELDS.some((f) => problemOf(f) !== ''));
const changed = computed(() => !!settings.value && !!form.value && FIELDS.some((f) => settings.value!.fleet[f.key] !== form.value![f.key]));

function showSettings(s: ServiceSettings) {
  settings.value = s;
  form.value = { ...s.fleet };
}

async function saveCapacity() {
  if (!settings.value || !form.value || busy.value || hasProblem.value) return;
  busy.value = true;
  const result = await saveSettings({ subscriptions_enabled: settings.value.subscriptions_enabled, ...form.value });
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  showSettings(result.value);
  flash('Capacity saved.');
}

onMounted(async () => {
  const result = await fetchSettings();
  if (result.ok) showSettings(result.value);
  else settingsError.value = result.error.message;
});
</script>
