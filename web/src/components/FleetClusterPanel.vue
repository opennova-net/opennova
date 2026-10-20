<template>
  <div class="fleet-cluster space-y-2 rounded-panel border border-border bg-panel p-4 text-sm">
    <p v-if="unread" class="text-ink-muted">ECS: {{ unread }}.</p>
    <p v-else-if="cluster?.error" class="text-danger">The fleet cluster could not be read: {{ cluster.error }}</p>
    <template v-else-if="cluster">
      <p class="flex flex-wrap items-baseline gap-x-2 gap-y-1">
        <span :class="toneBadgeClass(state.tone)">{{ state.label }}</span>
        <span>{{ summary }}</span>
      </p>
      <ul v-if="cluster.instances.length" class="space-y-1">
        <li v-for="i in cluster.instances" :key="i.id" class="fleet-cluster-instance text-xs text-ink-muted">
          <RouterLink v-if="i.machine_id" :to="`/admin/fleet/machines/${i.machine_id}`" class="text-accent hover:text-ink-bright">
            Machine {{ i.machine_id }}</RouterLink
          ><span v-else class="text-warn">A machine with no machine-id</span>
          <template v-if="i.kind"> ({{ i.kind === 'own' ? 'dedicated' : i.kind }})</template>:
          <span :class="i.agent_connected ? '' : 'font-semibold text-danger'">{{
            i.agent_connected ? 'its ECS agent connected' : 'its ECS agent NOT connected'
          }}</span>, {{ i.status.toLowerCase() }}, {{ countLabel(i.running_tasks, 'task') }} running<template v-if="i.pending_tasks"
            >, {{ i.pending_tasks }} starting</template
          ><template v-if="i.ec2_instance_id">, <span class="font-mono">{{ i.ec2_instance_id }}</span></template
          ><template v-if="i.image_id">, <span class="font-mono">{{ i.image_id }}</span></template>
        </li>
      </ul>
    </template>
  </div>
</template>

<script setup lang="ts">
// The fleet cluster as ECS sees it (GET /api/admin/infrastructure's
// cloud.fleet): hostd's daemon service, one task on each machine, and the
// machines as its container instances.
import { computed } from 'vue';
import { RouterLink } from 'vue-router';
import type { InfraFleetCluster } from '../types/infra';
import { countLabel, serviceState, toneBadgeClass, type Tone } from '../utils/infraView';

const props = defineProps<{
  cluster: InfraFleetCluster | null;
  // Why ECS was not read (the local stack, a failed read); '' when it was.
  unread: string;
}>();

const state = computed<{ label: string; tone: Tone }>(() => {
  const c = props.cluster;
  if (!c?.hostd) return { label: 'Missing', tone: 'bad' };
  if (c.instances.length === 0) return { label: 'No machines', tone: 'idle' };
  return serviceState(c.hostd);
});

const summary = computed(() => {
  const c = props.cluster;
  if (!c) return '';
  if (!c.hostd) return `hostd's daemon service is missing from ${c.cluster}.`;
  if (c.instances.length === 0) {
    return `No machine has joined ${c.cluster}, so hostd runs nowhere yet: it starts on each machine as the machine joins.`;
  }
  return `hostd runs on ${c.hostd.running} of ${countLabel(c.instances.length, 'machine')} in ${c.cluster} (${c.hostd.task_definition}).`;
});
</script>
