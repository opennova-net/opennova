<template>
  <article class="infra-service min-w-0 space-y-3 rounded-panel border border-border bg-panel p-4" :aria-labelledby="`infra-service-${name}`">
    <div class="flex items-start justify-between gap-3">
      <div class="min-w-0">
        <h3 :id="`infra-service-${name}`" class="text-lg font-semibold text-ink">
          <RouterLink v-if="to" :to="to" class="hover:text-ink-bright">{{ name }}</RouterLink>
          <template v-else>{{ name }}</template>
        </h3>
        <template v-if="service">
          <p class="break-all font-mono text-xs text-ink-muted">{{ service.task_definition }}</p>
          <p v-if="service.on_release !== null" class="text-xs" :class="service.on_release ? 'text-ink-muted' : 'text-warn'">
            {{ service.on_release ? "The release's revision" : "Not the release's revision" }}
          </p>
        </template>
        <p v-else-if="cloudRead" class="text-xs text-warn">ECS runs no service of it.</p>
      </div>
      <div class="flex shrink-0 flex-col items-end gap-1">
        <template v-if="service">
          <span :class="toneBadgeClass(serviceState(service).tone)">{{ serviceState(service).label }}</span>
          <span class="whitespace-nowrap text-sm text-ink">{{ service.running }} of {{ service.desired }} running</span>
          <span v-if="service.pending > 0" class="text-xs text-warn">{{ service.pending }} starting</span>
        </template>
        <span v-else-if="usage" class="whitespace-nowrap text-sm text-ink">{{ countLabel(usage.running, 'task') }} reporting</span>
      </div>
    </div>

    <!-- What it uses now and its tasks gone, from its own samples. -->
    <template v-if="usage">
      <p class="infra-program-usage text-sm" :class="reporting ? 'text-ink' : 'text-warn'">{{ usageLine(name, reporting ? usage : null) }}</p>
      <p class="infra-program-restarts text-xs" :class="usage.restarts_24h > 0 ? 'font-semibold text-warn' : 'text-ink-muted'">
        {{
          usage.restarts_24h === 0
            ? 'No task gone in the last 24 hours.'
            : `${countLabel(usage.restarts_24h, 'task')} gone in the last 24 hours (a crash, a deploy or a scale-in).`
        }}
      </p>
    </template>

    <template v-if="service">
      <ul v-if="showDeployments(service)" class="space-y-1 text-xs text-ink-muted">
        <li v-for="d in service.deployments" :key="d.id" class="infra-deployment">
          <span class="font-medium text-ink">{{ d.status === 'PRIMARY' ? 'New' : 'Old' }}</span> <span class="font-mono">{{
            d.task_definition
          }}</span>: {{ d.running }} of {{ d.desired }} running<template
            v-if="d.failed > 0"
            >, <span class="text-danger">{{ countLabel(d.failed, 'task') }} failed</span></template
          >, rollout {{ rolloutLabel(d.rollout_state).toLowerCase()
          }}<template v-if="d.created_at">, began <RelativeTime :value="d.created_at" :now="now" /></template>
        </li>
      </ul>
      <p v-else-if="primaryDeployment(service)?.created_at" class="text-xs text-ink-muted">
        Deployed <RelativeTime :value="primaryDeployment(service)!.created_at!" :now="now" />, rollout
        {{ rolloutLabel(primaryDeployment(service)!.rollout_state).toLowerCase() }}
      </p>

      <ul v-if="service.tasks.length" class="divide-y divide-border border-t border-border">
        <li v-for="t in service.tasks" :key="t.id" class="infra-task space-y-1 py-2 text-sm">
          <div class="flex flex-wrap items-center gap-x-3 gap-y-1">
            <span class="font-mono text-ink" :title="`Task ${t.id}`">{{ t.id.slice(0, 8) }}</span>
            <span :class="toneBadgeClass(healthLabel(t.health).tone)">{{ healthLabel(t.health).label }}</span>
            <span v-if="t.last_status !== 'RUNNING'" class="text-xs text-warn">{{ taskStatusLabel(t.last_status) }}</span>
            <span v-if="t.on_release === false" class="text-xs text-warn">Revision {{ revisionOf(t.task_definition) }}</span>
          </div>
          <p class="text-xs text-ink-muted">
            <template v-if="t.started_at">Started <RelativeTime :value="t.started_at" :now="now" /></template>
            <template v-else-if="t.created_at">Created <RelativeTime :value="t.created_at" :now="now" />, not started</template>
            <template v-if="t.zone">, {{ t.zone }}</template>
            <template v-if="t.private_ip">, <span class="font-mono">{{ t.private_ip }}</span></template>
            <template v-if="taskSize(t)">, {{ taskSize(t) }}</template>
          </p>
          <p v-for="c in t.containers" :key="c.name" class="break-all text-xs text-ink-muted">
            <span class="text-ink">{{ c.name }}</span>:
            <span class="font-mono" :title="c.image_digest ? `${c.image} (pulled ${c.image_digest})` : c.image">{{ shortImage(c.image) }}</span>
            <span v-if="c.health === 'UNHEALTHY'" class="text-danger">{{ ' unhealthy' }}</span>
          </p>
        </li>
      </ul>
      <p v-else class="border-t border-border pt-2 text-sm text-ink-muted">No task runs.</p>

      <div v-if="service.stopped_tasks.length" class="space-y-1">
        <h4 class="text-xs font-semibold uppercase tracking-wider text-ink-muted">Recently stopped</h4>
        <ul class="space-y-1">
          <li v-for="t in service.stopped_tasks" :key="t.id" class="infra-stopped-task text-xs text-ink-muted">
            <span class="font-mono text-ink" :title="`Task ${t.id}`">{{ t.id.slice(0, 8) }}</span>
            <template v-if="t.stopped_at"> stopped <RelativeTime :value="t.stopped_at" :now="now" /></template>:
            {{ stoppedReason(t) }}
          </li>
        </ul>
      </div>

      <details v-if="service.events.length" class="text-xs">
        <summary class="cursor-pointer text-accent hover:text-ink-bright">Recent events ({{ service.events.length }})</summary>
        <ul class="mt-2 space-y-1 text-ink-muted">
          <li v-for="(e, i) in service.events" :key="i" class="infra-event">
            <RelativeTime v-if="e.at" :value="e.at" :now="now" class="whitespace-nowrap" />: {{ e.message }}
          </li>
        </ul>
      </details>
    </template>

    <slot />

    <p v-if="to" class="border-t border-border pt-2 text-sm">
      <RouterLink :to="to" class="text-accent hover:text-ink-bright">{{ linkLabel }}<span class="sr-only">: {{ name }}</span></RouterLink>
    </p>
  </article>
</template>

<script setup lang="ts">
// One program on the Infrastructure page: its ECS service when AWS was read
// (its state, revision against the release, counts, deployments, tasks with
// their health and images, recently stopped tasks, events), what its reporting
// tasks use now in words ("web: 41 % CPU of 0.5 vCPU, 612 MiB of 1 GiB, ...")
// and its tasks gone in 24 hours (GET /api/admin/programs), opening its own
// page; the hostd card adds the fleet cluster's machines (its slot).
import { computed } from 'vue';
import { RouterLink } from 'vue-router';
import RelativeTime from './RelativeTime.vue';
import type { InfraService, InfraTask } from '../types/infra';
import type { ProgramUsage } from '../types/usage';
import {
  countLabel,
  healthLabel,
  primaryDeployment,
  rolloutLabel,
  serviceState,
  shortImage,
  taskSize,
  taskStatusLabel,
  toneBadgeClass,
} from '../utils/infraView';
import { usageLine } from '../utils/usageView';

const props = withDefaults(
  defineProps<{
    name: string;
    // ECS's view; null where it was not read, or runs no service of it.
    service: InfraService | null;
    // Its samples' view; null where none was read (hostd's).
    usage: ProgramUsage | null;
    // Whether AWS was read (a missing service is then one ECS lacks).
    cloudRead: boolean;
    now: number;
    // The page it opens, and the link's words.
    to?: string | null;
    linkLabel?: string;
  }>(),
  { to: null, linkLabel: 'Its usage and logs' },
);

const reporting = computed(() => (props.usage?.tasks ?? []).some((t) => t.reporting));

// The deployments listed while there is more than one, or the one is not done.
function showDeployments(s: InfraService): boolean {
  return s.deployments.length > 1 || (s.deployments.length === 1 && s.deployments[0].rollout_state !== 'COMPLETED');
}

// "43" from "opennova-prod-web:43".
function revisionOf(taskDefinition: string): string {
  return taskDefinition.slice(taskDefinition.lastIndexOf(':') + 1);
}

function stoppedReason(t: InfraTask): string {
  const exits = t.containers
    .filter((c) => c.exit_code !== null)
    .map((c) => `${c.name} exited with code ${c.exit_code}`);
  return [t.stopped_reason || 'No reason given', ...exits].join('; ');
}
</script>
