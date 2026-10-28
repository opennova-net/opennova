<template>
  <section class="space-y-4" aria-labelledby="fleet-roll-heading">
    <div class="flex flex-wrap items-center justify-between gap-3">
      <h2 id="fleet-roll-heading" class="section-heading">Roll</h2>
      <button type="button" class="btn-secondary btn-sm" :disabled="loading || busy" @click="load">
        {{ loading ? 'Reading…' : 'Read again' }}
      </button>
    </div>

    <LiveNotices :message="message" :error="error" />

    <template v-if="plan">
      <p class="roll-image text-sm text-ink-muted">
        Templates' image <span class="font-mono text-ink">{{ plan.image_id || 'none' }}</span>
      </p>
      <div class="table-wrap">
        <table class="data-table">
          <caption class="sr-only">The roll's machines</caption>
          <thead>
            <tr>
              <th scope="col"><span class="sr-only">Roll it</span></th>
              <th scope="col">Machine</th>
              <th scope="col">Image</th>
              <th scope="col">Servers</th>
              <th scope="col">Plan</th>
            </tr>
          </thead>
          <tbody>
            <TableState v-if="plan.machines.length === 0" :colspan="5" :rows="plan.machines" empty="Nothing to roll." />
            <tr v-for="m in plan.machines" :key="m.id" class="roll-row">
              <td>
                <input
                  :id="`roll-pick-${m.id}`"
                  v-model="picked"
                  type="checkbox"
                  :value="m.id"
                  :disabled="busy"
                  class="h-4 w-4 accent-accent"
                  :aria-label="`Roll machine ${m.id}`"
                />
              </td>
              <th scope="row">
                <MachineLink :id="m.id" />
                <span class="block text-xs font-normal text-ink-muted">{{ machineKindLabel(m.kind) }} · {{ machineStateLabel(m.state).toLowerCase() }}</span>
              </th>
              <td class="whitespace-nowrap font-mono text-xs">{{ m.image_id || '—' }}</td>
              <td>{{ m.servers }}</td>
              <td>
                <Badge :tone="m.action === 'replace' ? 'warn' : 'ok'">{{ m.action === 'replace' ? 'Replace' : 'Keep' }}</Badge>
                <span v-if="m.why" class="block text-xs text-ink-muted">{{ m.why }}</span>
              </td>
            </tr>
          </tbody>
        </table>
      </div>

      <form v-if="plan.machines.length" class="flex flex-wrap items-end gap-3" @submit.prevent="roll">
        <p class="w-full text-sm" :class="picked.length ? 'text-warn' : 'text-ink-muted'">{{ pickedText }}</p>
        <div>
          <label for="roll-confirm" class="field-label">Type <span class="font-mono font-semibold">{{ ROLL_PHRASE }}</span></label>
          <input
            id="roll-confirm"
            v-model="typedPhrase"
            type="text"
            autocomplete="off"
            autocapitalize="characters"
            spellcheck="false"
            class="field w-40 font-mono"
          />
        </div>
        <button type="submit" class="btn-warn" :disabled="busy || picked.length === 0 || typedPhrase.trim() !== ROLL_PHRASE">
          {{ busy ? 'Rolling…' : `Roll ${countLabel(picked.length, 'machine')}` }}
        </button>
      </form>
    </template>
  </section>
</template>

<script setup lang="ts">
// The Roll tab: the preview (GET /api/admin/fleet/roll), each machine that
// takes servers with its image and replace or keep, the preview's choice
// picked to start with; the roll of the machines picked (POST
// /api/admin/fleet/roll) behind a typed confirmation.
import { computed, onMounted, ref } from 'vue';
import MachineLink from './links/MachineLink.vue';
import Badge from './ui/Badge.vue';
import LiveNotices from './ui/LiveNotices.vue';
import TableState from './ui/TableState.vue';
import { fetchRollPlan, rollFleet } from '../api/admin';
import type { RollPlan } from '../types/hostedServer';
import { machineKindLabel, machineStateLabel } from '../utils/adminView';
import { countLabel } from '../utils/infraView';

const emit = defineEmits<{
  // A roll went in: the fleet's lists change.
  (e: 'rolled'): void;
}>();

const ROLL_PHRASE = 'ROLL';

const plan = ref<RollPlan | null>(null);
const picked = ref<string[]>([]);
const typedPhrase = ref('');
const loading = ref(false);
const busy = ref(false);
const message = ref('');
const error = ref('');

const pickedServers = computed(() =>
  (plan.value?.machines ?? []).filter((m) => picked.value.includes(m.id)).reduce((n, m) => n + m.servers, 0),
);
// "1 machine picked, 2 servers move."
const pickedText = computed(() => {
  const n = picked.value.length;
  if (n === 0) return 'None picked.';
  return `${countLabel(n, 'machine')} picked, ${countLabel(pickedServers.value, 'server')} move.`;
});

async function load() {
  loading.value = true;
  const result = await fetchRollPlan();
  loading.value = false;
  if (!result.ok) {
    error.value = result.error.message;
    return;
  }
  error.value = '';
  plan.value = result.value;
  picked.value = [...result.value.replace];
}

async function roll() {
  if (busy.value || picked.value.length === 0 || typedPhrase.value.trim() !== ROLL_PHRASE) return;
  busy.value = true;
  const result = await rollFleet(picked.value);
  busy.value = false;
  if (!result.ok) {
    error.value = result.error.message;
    message.value = '';
    return;
  }
  const n = result.value;
  message.value = `${countLabel(n, 'machine')} draining.`;
  error.value = '';
  typedPhrase.value = '';
  emit('rolled');
  await load();
}

onMounted(load);
</script>
