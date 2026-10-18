<template>
  <section class="space-y-4" aria-labelledby="fleet-roll-heading">
    <div class="flex flex-col gap-3 sm:flex-row sm:items-center sm:justify-between">
      <h2 id="fleet-roll-heading" class="text-xl font-semibold text-ink">Roll the fleet</h2>
      <button type="button" :class="secondaryButton" :disabled="loading || busy" @click="load">
        {{ loading ? 'Reading…' : 'Read again' }}
      </button>
    </div>
    <p class="text-sm text-ink-muted">
      A roll is for a new machine image, launch template or user-data: each machine picked drains, and its servers move
      one at a time to machines launched from the templates as they are now, no restart counted. hostd and the serve image
      reach machines in place, without a roll.
    </p>

    <p role="status" class="sr-only">{{ message }}</p>
    <p role="alert" class="sr-only">{{ error }}</p>
    <p v-if="message" aria-hidden="true" class="rounded-panel border border-online/40 bg-online/10 p-3 text-sm text-online">{{ message }}</p>
    <p v-if="error" aria-hidden="true" class="rounded-panel border border-danger/40 bg-danger/10 p-3 text-sm text-danger">{{ error }}</p>

    <template v-if="plan">
      <p class="roll-image text-sm text-ink">
        <template v-if="plan.image_id">The launch templates pin <span class="font-mono">{{ plan.image_id }}</span>.</template>
        <template v-else>No image is pinned here, so every machine reads as one to replace.</template>
      </p>
      <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
        <table class="min-w-full divide-y divide-border">
          <caption class="sr-only">The roll's machines</caption>
          <thead>
            <tr class="bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted">
              <th scope="col" class="px-4 py-3">Roll it</th>
              <th scope="col" class="px-4 py-3">Machine</th>
              <th scope="col" class="px-4 py-3">Image</th>
              <th scope="col" class="px-4 py-3">Servers</th>
              <th scope="col" class="px-4 py-3">The preview says</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border text-sm text-ink">
            <tr v-if="plan.machines.length === 0">
              <td colspan="5" class="px-4 py-6 text-center text-ink-muted">No machine takes servers, so there is nothing to roll.</td>
            </tr>
            <tr v-for="m in plan.machines" :key="m.id" class="roll-row align-top hover:bg-hover">
              <td class="px-4 py-3">
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
              <th scope="row" class="whitespace-nowrap px-4 py-3 text-left font-normal">
                <RouterLink :to="`/admin/fleet/machines/${m.id}`" class="text-accent hover:text-ink-bright">Machine {{ m.id }}</RouterLink>
                <span class="block text-xs text-ink-muted">{{ machineKindLabel(m.kind) }}, {{ machineStateLabel(m.state).toLowerCase() }}</span>
              </th>
              <td class="whitespace-nowrap px-4 py-3 font-mono text-xs">{{ m.image_id || 'Not known' }}</td>
              <td class="whitespace-nowrap px-4 py-3">{{ m.servers }}</td>
              <td class="px-4 py-3">
                <span :class="toneBadgeClass(m.action === 'replace' ? 'warn' : 'ok')">{{ m.action === 'replace' ? 'Replace' : 'Keep' }}</span>
                <span v-if="m.why" class="block text-xs text-ink-muted">{{ m.why }}</span>
              </td>
            </tr>
          </tbody>
        </table>
      </div>

      <form v-if="plan.machines.length" class="space-y-3" @submit.prevent="roll">
        <p class="text-sm font-semibold" :class="picked.length ? 'text-warn' : 'text-ink-muted'">
          {{ pickedText }}
        </p>
        <label for="roll-confirm" class="block text-sm text-ink">
          Type <span class="font-mono font-semibold">{{ ROLL_PHRASE }}</span> to confirm
        </label>
        <input
          id="roll-confirm"
          v-model="typedPhrase"
          type="text"
          autocomplete="off"
          autocapitalize="characters"
          spellcheck="false"
          class="w-full max-w-xs rounded-control border border-border bg-surface px-3 py-2 font-mono text-sm text-ink focus:border-warn focus:outline-none"
        />
        <button
          type="submit"
          :disabled="busy || picked.length === 0 || typedPhrase.trim() !== ROLL_PHRASE"
          class="rounded-control bg-warn px-4 py-2 text-sm font-semibold text-on-accent transition hover:bg-warn/90 disabled:opacity-40"
        >
          {{ busy ? 'Rolling…' : `Roll ${countLabel(picked.length, 'machine')}` }}
        </button>
      </form>
    </template>
  </section>
</template>

<script setup lang="ts">
// The Fleet page's roll tab: the preview (GET /api/admin/fleet/roll), each
// machine that takes servers with its image against the templates' and replace
// or keep, the preview's choice picked to start with; the roll of the machines
// picked (POST /api/admin/fleet/roll {"machines": [...]}) behind a typed
// confirmation.
import { computed, onMounted, ref } from 'vue';
import { RouterLink } from 'vue-router';
import { fetchRollPlan, rollFleet } from '../api/admin';
import type { RollPlan } from '../types/hostedServer';
import { machineKindLabel, machineStateLabel } from '../utils/adminView';
import { countLabel, toneBadgeClass } from '../utils/infraView';

const emit = defineEmits<{
  // A roll went in: the fleet's lists change.
  (e: 'rolled'): void;
}>();

const ROLL_PHRASE = 'ROLL';

const secondaryButton =
  'rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40';

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
const pickedText = computed(() => {
  const n = picked.value.length;
  if (n === 0) return 'No machine picked.';
  const servers = pickedServers.value;
  return `${countLabel(n, 'machine')} picked: ${servers === 0 ? 'no server runs on them' : `${countLabel(servers, 'server')} move, each down while it moves`}.`;
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
  message.value = `${countLabel(n, 'machine')} ${n === 1 ? 'is' : 'are'} draining: ${n === 1 ? 'its' : 'their'} servers move to fresh machines one at a time.`;
  error.value = '';
  typedPhrase.value = '';
  emit('rolled');
  await load();
}

onMounted(load);
</script>
