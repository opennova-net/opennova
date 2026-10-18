<template>
  <section class="space-y-4" :aria-labelledby="`hosted-server-${server.id}-logs`">
    <h2 :id="`hosted-server-${server.id}-logs`" class="text-lg font-semibold text-ink">Logs</h2>
    <p v-if="newest === 0" class="text-sm text-ink-muted">The server has not started yet, so it has no log.</p>
    <template v-else>
      <label class="block max-w-xs space-y-1">
        <span class="block text-xs font-medium text-ink-muted">Start</span>
        <select
          :id="`hosted-server-${server.id}-start`"
          v-model.number="picked"
          class="w-full rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none"
        >
          <option v-for="g in generations" :key="g" :value="g">{{ startLabel(g) }}</option>
        </select>
      </label>
      <p class="text-xs text-ink-muted">
        Each start (a restart, a move to another machine, an update) writes a log of its own, kept 14 days.
      </p>
      <LogViewer
        :id-base="`hosted-server-${server.id}-log`"
        subject="server"
        :id="server.id"
        :generation="picked"
        :heading="`Start ${picked}'s output`"
        :followable="server.live && picked === newest"
      />
    </template>
  </section>
</template>

<script setup lang="ts">
// An admin's fleet server page's Logs tab (GET /api/admin/logs?subject=server):
// the output of one of its starts (its generations, newest first; the newest
// followed while the server lives). Owners never see it.
import { computed, ref, watch } from 'vue';
import LogViewer from '../../components/LogViewer.vue';
import type { FleetServer, HostedServer } from '../../types/hostedServer';

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
const newest = computed(() => (props.server as FleetServer).generation ?? 0);
const generations = computed(() => Array.from({ length: newest.value }, (_, i) => newest.value - i));
const picked = ref(newest.value);

// A new start (a restart, a move): follow it, unless an older one was picked.
watch(newest, (now, before) => {
  if (picked.value === before || picked.value === 0) picked.value = now;
});

function startLabel(g: number): string {
  return g === newest.value ? `Start ${g}, the newest` : `Start ${g}`;
}
</script>
