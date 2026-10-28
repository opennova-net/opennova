<template>
  <article class="machine-card card min-w-0 space-y-3 p-4" :class="trouble ? 'border-danger/40' : ''" :aria-labelledby="`machine-${machine.id}-heading`">
    <div class="flex items-start justify-between gap-3">
      <div class="min-w-0">
        <h3 :id="`machine-${machine.id}-heading`" class="section-heading">
          <MachineLink :id="machine.id" />
        </h3>
        <p class="text-xs text-ink-muted">
          {{ machineKindLabel(machine.kind) }}<template v-if="machine.instance_type"> · <span class="font-mono">{{ machine.instance_type }}</span></template>
        </p>
      </div>
      <Badge :tone="machineTone(machine.state)">{{ machineStateLabel(machine.state) }}</Badge>
    </div>

    <ul v-if="flags.length" class="space-y-0.5 text-sm">
      <li v-for="f in flags" :key="f.text" class="machine-flag" :class="f.bad ? 'text-danger' : 'text-warn'">{{ f.text }}</li>
    </ul>
    <p v-if="report" class="machine-report text-xs" :class="report.late ? 'font-semibold text-danger' : 'text-ink-muted'">{{ report.text }}</p>

    <dl v-if="figures.length" class="machine-figures grid grid-cols-[auto_1fr] gap-x-4 gap-y-0.5 text-sm">
      <template v-for="f in figures" :key="f.label">
        <dt class="text-ink-muted">{{ f.label }}</dt>
        <dd :class="f.flag ? 'font-semibold text-warn' : 'text-ink'">{{ f.value }}<template v-if="f.flag"> ({{ f.flag }})</template></dd>
      </template>
    </dl>

    <div v-if="live" class="space-y-1">
      <p class="text-xs text-ink-muted">{{ machine.slots_used }}/{{ machine.slots }} slots</p>
      <SlotStrip :machine-id="machine.id" :slots="machine.slots" :holders="machine.slot_holders" :servers="servers" />
    </div>

    <p class="machine-address text-xs text-ink-muted">
      <span class="font-mono text-ink">{{ machine.public_address || 'No address' }}</span>
      <template v-if="machine.address"> · static <span class="font-mono text-ink">{{ machine.address.name }}</span></template>
      · {{ formatBytes(machine.egress_bytes_month) }} sent this month
    </p>
    <p v-if="!live && machine.end_reason" class="text-xs text-ink-muted">{{ machineStateLabel(machine.state) }}: {{ machine.end_reason }}</p>

    <div v-if="canDrain || canEnd" class="flex flex-wrap gap-1 border-t border-border pt-2">
      <button v-if="canDrain" type="button" class="btn-ghost btn-sm" :disabled="busy" @click="emit('drain')">
        Drain<span class="sr-only"> machine {{ machine.id }}</span>
      </button>
      <button v-if="canEnd" type="button" class="btn-ghost btn-sm" :disabled="busy" @click="emit('end')">
        End<span class="sr-only"> machine {{ machine.id }}</span>
      </button>
    </div>
  </article>
</template>

<script setup lang="ts">
// One fleet machine on the Machines tab: its kind, type and state, what is
// wrong with it, what it uses now, its slots naming the servers in them, its
// address, and drain and end while it lives (the page confirms them).
import { computed } from 'vue';
import SlotStrip from './SlotStrip.vue';
import MachineLink from './links/MachineLink.vue';
import Badge from './ui/Badge.vue';
import type { FleetMachine, FleetServer } from '../types/hostedServer';
import { machineKindLabel, machineStateLabel } from '../utils/adminView';
import { formatBytes } from '../utils/format';
import { machineFigures, machineFlags, machineLive, machineTone, reportAge } from '../utils/machineView';

const props = defineProps<{
  machine: FleetMachine;
  // The servers the page has read, to name the ones in its slots.
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
const canDrain = computed(() => props.machine.state === 'ready' || props.machine.state === 'booting');
const canEnd = computed(() => !['ended', 'ending', 'failed'].includes(props.machine.state));
</script>
