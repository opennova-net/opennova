<template>
  <section class="space-y-4" :aria-labelledby="`hosted-server-${server.id}-machine`">
    <h2 :id="`hosted-server-${server.id}-machine`" class="text-lg font-semibold text-ink">Machine</h2>
    <p v-if="!fleet.machine_ref" class="text-sm text-ink-muted">
      {{ server.live ? 'The server is not placed on a machine yet: it waits for a slot.' : 'The server has ended; its slot is free.' }}
    </p>
    <dl v-else class="grid gap-x-6 gap-y-2 rounded-panel border border-border bg-panel p-4 text-sm sm:grid-cols-[auto_1fr]">
      <dt class="text-ink-muted">Machine</dt>
      <dd class="break-all font-mono text-ink">{{ fleet.machine_ref }}</dd>
      <dt class="text-ink-muted">Slot</dt>
      <dd class="text-ink">{{ fleet.slot ?? 'None' }}</dd>
      <dt class="text-ink-muted">Game port</dt>
      <dd class="font-mono text-ink">{{ fleet.game_port ?? 'None' }}</dd>
      <template v-if="machine">
        <dt class="text-ink-muted">Kind</dt>
        <dd class="text-ink">{{ machineKindLabel(machine.kind) }}</dd>
        <dt class="text-ink-muted">State</dt>
        <dd class="text-ink">{{ machineStateLabel(machine.state) }}</dd>
        <dt class="text-ink-muted">Instance type</dt>
        <dd class="font-mono text-ink">{{ machine.instance_type || 'Unknown' }}</dd>
        <dt class="text-ink-muted">Servers on it</dt>
        <dd class="text-ink">{{ machine.slots_used }} of {{ machine.slots }} slots</dd>
        <dt class="text-ink-muted">Address</dt>
        <dd class="break-all font-mono text-ink">{{ machine.public_address || 'None yet' }}</dd>
        <dt class="text-ink-muted">Data sent this month</dt>
        <dd class="text-ink">{{ formatBytes(machine.egress_bytes_month) }}, all its servers together</dd>
      </template>
    </dl>
    <p v-if="fleet.machine_ref && !machine" class="text-sm text-ink-muted">The machine's own record did not come with the server.</p>
    <RouterLink to="/admin/fleet" class="inline-block text-sm text-accent hover:text-ink-bright">The whole fleet</RouterLink>
  </section>
</template>

<script setup lang="ts">
// An admin's fleet server page's Machine tab: where the server runs (its
// machine, slot and game port) and that machine's own record, from GET
// /api/admin/hosted/<id>.
import { computed } from 'vue';
import { RouterLink } from 'vue-router';
import type { FleetServer, HostedServer } from '../../types/hostedServer';
import { machineKindLabel, machineStateLabel } from '../../utils/adminView';
import { formatBytes } from '../../utils/format';

const props = defineProps<{
  server: HostedServer;
  scope: 'owner' | 'admin';
  now: number;
}>();

defineEmits<{
  (e: 'changed', server: HostedServer | null): void;
  (e: 'gone'): void;
  (e: 'session-ended'): void;
}>();

// The admin's page reads a FleetServer.
const fleet = computed(() => props.server as FleetServer);
const machine = computed(() => fleet.value.machine ?? null);
</script>
