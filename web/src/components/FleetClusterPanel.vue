<template>
  <section class="fleet-cluster space-y-2" aria-labelledby="fleet-cluster-heading">
    <h2 id="fleet-cluster-heading" class="section-heading flex flex-wrap items-center gap-2">
      ECS
      <Badge v-if="cluster && !cluster.error && !unread" :tone="state.tone">{{ state.label }}</Badge>
      <span v-if="summary" class="text-sm font-normal text-ink-muted">{{ summary }}</span>
    </h2>
    <p v-if="unread" class="text-sm text-ink-muted">Not read: {{ unread }}.</p>
    <Notice v-else-if="cluster?.error" tone="bad">{{ cluster.error }}</Notice>
    <div v-else-if="cluster && cluster.instances.length" class="table-wrap">
      <table class="data-table">
        <caption class="sr-only">The fleet cluster's machines</caption>
        <thead>
          <tr>
            <th scope="col">Machine</th>
            <th scope="col">ECS agent</th>
            <th scope="col">Status</th>
            <th scope="col">Tasks</th>
            <th scope="col">Instance</th>
            <th scope="col">Image</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="i in cluster.instances" :key="i.id" class="fleet-cluster-instance">
            <th scope="row">
              <MachineLink v-if="i.machine_id" :id="i.machine_id" />
              <span v-else class="text-warn">No machine-id</span>
              <span v-if="i.kind" class="ml-2 text-xs text-ink-muted">{{ i.kind === 'own' ? 'dedicated' : i.kind }}</span>
            </th>
            <td><Badge :tone="i.agent_connected ? 'ok' : 'bad'">{{ i.agent_connected ? 'Connected' : 'Not connected' }}</Badge></td>
            <td>{{ i.status.toLowerCase() }}</td>
            <td>{{ i.running_tasks }}<template v-if="i.pending_tasks"> (+{{ i.pending_tasks }} starting)</template></td>
            <td class="font-mono text-xs">{{ i.ec2_instance_id || '—' }}</td>
            <td class="font-mono text-xs">{{ i.image_id || '—' }}</td>
          </tr>
        </tbody>
      </table>
    </div>
  </section>
</template>

<script setup lang="ts">
// The fleet cluster as ECS sees it (GET /api/admin/infrastructure's
// cloud.fleet): hostd's daemon service and the machines as its container
// instances.
import { computed } from 'vue';
import MachineLink from './links/MachineLink.vue';
import Badge from './ui/Badge.vue';
import Notice from './ui/Notice.vue';
import type { InfraFleetCluster } from '../types/infra';
import { serviceState } from '../utils/infraView';
import type { Tone } from '../utils/tone';

const props = defineProps<{
  cluster: InfraFleetCluster | null;
  // Why ECS was not read (the local stack, a failed read); '' when it was.
  unread: string;
}>();

const state = computed<{ label: string; tone: Tone }>(() => {
  const c = props.cluster;
  if (!c?.hostd) return { label: 'hostd missing', tone: 'bad' };
  if (c.instances.length === 0) return { label: 'No machines', tone: 'muted' };
  return serviceState(c.hostd);
});

// "hostd 1/1 · opennova-prod-fleet · opennova-prod-hostd:5"
const summary = computed(() => {
  const c = props.cluster;
  if (!c || c.error || props.unread) return '';
  if (!c.hostd) return c.cluster;
  return [`hostd ${c.hostd.running}/${c.instances.length}`, c.cluster, c.hostd.task_definition].join(' · ');
});
</script>
