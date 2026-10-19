<template>
  <UsageCharts
    :key="server.id"
    :id-base="`hosted-server-${server.id}-usage`"
    subject="server"
    :id="server.id"
    :name="`Server ${server.id}`"
    heading="Usage"
    :heading-level="2"
    :follow="server.live"
  />
</template>

<script setup lang="ts">
// An admin's fleet server page's Usage tab (GET /api/admin/usage?subject=server):
// what its container uses of its slot (CPU, memory, network in and out) and
// its tick health (its frames' 99th percentile time, its late ticks), polled
// while it is live. Owners never see it.
import UsageCharts from '../../components/UsageCharts.vue';
import type { HostedServer } from '../../types/hostedServer';

defineProps<{
  server: HostedServer;
  scope: 'owner' | 'admin';
  now: number;
}>();

defineEmits<{
  (e: 'changed', server: HostedServer | null): void;
  (e: 'gone'): void;
  (e: 'session-ended'): void;
}>();
</script>
