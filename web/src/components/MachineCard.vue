<template>
  <article class="machine-card min-w-0 space-y-3 rounded-panel border bg-panel p-4" :class="trouble ? 'border-danger/40' : 'border-border'" :aria-labelledby="`machine-${machine.id}-heading`">
    <div class="flex items-start justify-between gap-3">
      <div class="min-w-0">
        <h3 :id="`machine-${machine.id}-heading`" class="text-lg font-semibold text-ink">
          <RouterLink :to="`/admin/fleet/machines/${machine.id}`" class="hover:text-ink-bright">Machine {{ machine.id }}</RouterLink>
        </h3>
        <p class="text-xs text-ink-muted">
          {{ machineKindLabel(machine.kind) }}<template v-if="machine.instance_type">, <span class="font-mono">{{ machine.instance_type }}</span></template>
          <span v-if="machine.cloud_id" class="block font-mono">{{ machine.cloud_id }}</span>
        </p>
      </div>
      <span :class="toneBadgeClass(machineTone(machine.state))">{{ machineStateLabel(machine.state) }}</span>
    </div>

    <!-- What is wrong, each in words and colour. -->
    <ul v-if="flags.length" class="space-y-1 text-sm">
      <li v-for="f in flags" :key="f.text" class="machine-flag" :class="f.bad ? 'text-danger' : 'text-warn'">{{ f.text }}</li>
    </ul>
    <p v-if="report" class="machine-report text-xs" :class="report.late ? 'font-semibold text-danger' : 'text-ink-muted'">{{ report.text }}</p>

    <dl v-if="figures.length" class="machine-figures grid grid-cols-[auto_1fr] gap-x-4 gap-y-1 text-sm">
      <template v-for="f in figures" :key="f.label">
        <dt class="text-ink-muted">{{ f.label }}</dt>
        <dd :class="f.flag ? 'font-semibold text-warn' : 'text-ink'">{{ f.value }}<template v-if="f.flag">, {{ f.flag }}</template></dd>
      </template>
    </dl>
    <p v-else-if="live" class="machine-figures text-sm text-ink-muted">Use: not reported yet.</p>

    <div v-if="live" class="space-y-1">
      <p class="text-xs text-ink-muted">{{ machine.slots_used }} of {{ machine.slots }} slots in use</p>
      <SlotStrip :machine-id="machine.id" :slots="machine.slots" :holders="machine.slot_holders" :servers="servers" />
    </div>

    <p class="text-xs text-ink-muted">
      <template v-if="machine.public_address">Address <span class="font-mono text-ink">{{ machine.public_address }}</span></template>
      <template v-else>No address yet</template>
      <template v-if="machine.address">, the static address <span class="font-mono text-ink">{{ machine.address.name }}</span></template>.
      {{ formatBytes(machine.egress_bytes_month) }} sent this month.
    </p>
    <p v-if="!live && machine.end_reason" class="text-xs text-ink-muted">{{ machineStateLabel(machine.state) }}: {{ machine.end_reason }}</p>

    <div class="flex flex-wrap items-center gap-3 border-t border-border pt-3 text-sm">
      <RouterLink :to="`/admin/fleet/machines/${machine.id}`" class="text-accent hover:text-ink-bright">
        Open<span class="sr-only"> machine {{ machine.id }}</span>
      </RouterLink>
      <button
        v-if="machine.state === 'ready' || machine.state === 'booting'"
        type="button"
        :disabled="busy"
        class="text-warn/90 hover:text-warn disabled:opacity-40"
        @click="emit('drain')"
      >
        Drain<span class="sr-only"> machine {{ machine.id }}</span>
      </button>
      <button
        v-if="machine.state !== 'ended' && machine.state !== 'ending' && machine.state !== 'failed'"
        type="button"
        :disabled="busy"
        class="text-danger/80 hover:text-danger disabled:opacity-40"
        @click="emit('end')"
      >
        End<span class="sr-only"> machine {{ machine.id }}</span>
      </button>
    </div>
  </article>
</template>

<script setup lang="ts">
// One fleet machine on the Fleet page: its kind, type and state; what is wrong
// with it, said in words (its last report late past 30 s, lost and held by the
// mass-lost brake, a protocol mismatch held, older than the release); what it
// uses now against the health check's thresholds (machine_json's stats); its
// slots (SlotStrip); its address and static address; drain and end while it
// lives, and its own page.
import { computed } from 'vue';
import { RouterLink } from 'vue-router';
import SlotStrip from './SlotStrip.vue';
import type { FleetMachine, FleetServer } from '../types/hostedServer';
import { machineKindLabel, machineStateLabel } from '../utils/adminView';
import { formatBytes } from '../utils/format';
import { toneBadgeClass } from '../utils/infraView';
import { machineFigures, machineFlags, machineLive, machineTone, reportAge } from '../utils/machineView';

const props = defineProps<{
  machine: FleetMachine;
  // The servers the page has read on it (their names and ticks).
  servers: readonly FleetServer[];
  now: number;
  busy?: boolean;
}>();

const emit = defineEmits<{
  (e: 'drain'): void;
  (e: 'end'): void;
}>();

const live = computed(() => machineLive(props.machine.state));
const report = computed(() => reportAge(props.machine, props.now));
const figures = computed(() => (live.value ? machineFigures(props.machine.stats) : []));

const flags = computed(() => machineFlags(props.machine, props.now));

const trouble = computed(() => flags.value.some((f) => f.bad) || report.value?.late === true);
</script>
