<template>
  <StatsDashboard
    :key="server.id"
    :base-id="`hosted-server-${server.id}-stats`"
    heading="Stats"
    :description="
      scope === 'owner'
        ? 'Sampled once a minute while your server is live: its players, its availability and the data it sends.'
        : 'Sampled once a minute while the server is live.'
    "
    variant="server"
    :follow="server.live"
    :load="load"
    @session-ended="emit('session-ended')"
  />
</template>

<script setup lang="ts">
// A hosted server page's Stats tab, the owner's (GET
// /api/me/servers/<id>/stats) and the admin's (GET
// /api/admin/hosted/<id>/stats): StatsDashboard over the server's stats,
// polled each minute while it is live.
import StatsDashboard from '../../components/StatsDashboard.vue';
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
