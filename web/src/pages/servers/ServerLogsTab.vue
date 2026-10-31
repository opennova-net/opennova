<template>
  <div class="space-y-3">
    <p v-if="newest === 0" class="text-sm text-ink-muted">No log yet.</p>
    <template v-else>
      <div class="flex items-center gap-2">
        <label :for="`hosted-server-${server.id}-start`" class="text-sm text-ink-muted">Start</label>
        <select
          :id="`hosted-server-${server.id}-start`"
          v-model.number="picked"
          class="field w-auto"
          title="Each start (a restart, a move, an update) has its own log, kept 14 days"
        >
          <option v-for="g in generations" :key="g" :value="g">{{ g === newest ? `${g} (newest)` : g }}</option>
        </select>
      </div>
      <LogViewer
        :id-base="`hosted-server-${server.id}-log`"
        subject="server"
        :id="server.id"
        :generation="picked"
        :heading="`Start ${picked}`"
        :heading-level="2"
        :followable="server.live && picked === newest"
      />
    </template>
  </div>
</template>

<script setup lang="ts">
// A server page's Logs tab for an admin, on the admin's page and on the
// owner's page of an admin's session (GET /api/admin/logs?subject=server): the
// output of one of its starts (its generations, newest first; the newest
// followed while the server lives).
import { computed, ref, watch } from 'vue';
import LogViewer from '../../components/LogViewer.vue';
import type { HostedServer } from '../../types/hostedServer';

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

const newest = computed(() => props.server.generation);
const generations = computed(() => Array.from({ length: newest.value }, (_, i) => newest.value - i));
const picked = ref(newest.value);

// A new start (a restart, a move): follow it, unless an older one was picked.
watch(newest, (now, before) => {
  if (picked.value === before || picked.value === 0) picked.value = now;
});
</script>
