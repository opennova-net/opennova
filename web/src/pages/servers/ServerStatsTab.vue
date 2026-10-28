<template>
  <div class="space-y-8">
    <StatsDashboard
      :key="server.id"
      :base-id="`hosted-server-${server.id}-stats`"
      heading="Stats"
      variant="server"
      :follow="server.live"
      :load="load"
      @session-ended="emit('session-ended')"
    />
    <UsageCharts
      v-if="scope === 'admin'"
      :key="`usage-${server.id}`"
      :id-base="`hosted-server-${server.id}-usage`"
      subject="server"
      :id="server.id"
      :name="`Server ${server.id}`"
      heading="Usage"
      :heading-level="2"
      :follow="server.live"
    />
  </div>
</template>

<script setup lang="ts">
// A hosted server page's Stats tab: StatsDashboard over its stats (the
// owner's GET /api/me/servers/<id>/stats, the admin's GET
// /api/admin/hosted/<id>/stats), and for an admin what its container uses of
// its slot (UsageCharts, GET /api/admin/usage?subject=server).
import StatsDashboard from '../../components/StatsDashboard.vue';
import UsageCharts from '../../components/UsageCharts.vue';
import { fetchServerStats } from '../../api/hostedStats';
import type { HostedServer } from '../../types/hostedServer';
import type { StatsRange } from '../../types/hostedStats';

const props = defineProps<{
  server: HostedServer;
  scope: 'owner' | 'admin';
  now: number;
}>();

const emit = defineEmits<{
  (e: 'changed', server: HostedServer | null): void;
  (e: 'gone'): void;
  (e: 'session-ended'): void;
}>();

function load(range: StatsRange, signal: AbortSignal) {
  return fetchServerStats(props.scope, props.server.id, range, signal);
}
</script>
