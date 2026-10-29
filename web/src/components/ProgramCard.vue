<template>
  <article class="infra-service card min-w-0 space-y-3 p-4" :aria-labelledby="`infra-service-${name}`">
    <div class="flex items-start justify-between gap-3">
      <div class="min-w-0">
        <h3 :id="`infra-service-${name}`" class="text-lg font-semibold text-ink">
          <RouterLink v-if="to" :to="to" class="entity-link">{{ name }}</RouterLink>
          <template v-else>{{ name }}</template>
        </h3>
        <!-- The build its tasks run: the headline; the ECS task revision a small note. -->
        <p v-if="builds.builds.length || builds.unknown" class="infra-program-build text-sm">
          <template v-for="(b, i) in builds.builds" :key="`${b.build.version} ${b.build.commit}`">
            <template v-if="i > 0">, </template>
            <BuildText :build="b.build" />
            <span v-if="many" class="text-xs text-ink-muted"> ({{ countLabel(b.count, 'task') }})</span>
          </template>
          <span v-if="builds.unknown" class="text-xs text-warn">
            <template v-if="builds.builds.length">, </template>build not known for {{ countLabel(builds.unknown, 'task') }}</span
          >
        </p>
        <template v-if="service">
          <p class="infra-program-revision text-xs text-ink-muted">
            {{ revisionLabel(service.task_definition) ?? service.task_definition
            }}<span v-if="service.on_release === false" class="text-warn">, not the release's</span>
          </p>
        </template>
        <p v-else-if="cloudRead" class="text-xs text-warn">No ECS service</p>
      </div>
      <div class="flex shrink-0 flex-col items-end gap-1">
        <template v-if="service">
          <span :class="badgeClass(serviceState(service).tone)">{{ serviceState(service).label }}</span>
          <span class="whitespace-nowrap text-sm text-ink">{{ service.running }} of {{ service.desired }} running</span>
          <span v-if="service.pending > 0" class="text-xs text-warn">{{ service.pending }} starting</span>
        </template>
        <span v-else-if="usage" class="whitespace-nowrap text-sm text-ink">{{ countLabel(usage.running, 'task') }} reporting</span>
      </div>
    </div>

    <!-- What it uses now and its tasks gone, from its own samples. -->
    <template v-if="usage">
      <p class="infra-program-usage text-sm" :class="reporting ? 'text-ink' : 'text-warn'">{{ usageLine(name, reporting ? usage : null) }}</p>
      <p v-if="usage.restarts_24h > 0" class="infra-program-restarts text-xs font-semibold text-warn">{{ countLabel(usage.restarts_24h, 'task') }} gone in 24 hours</p>
    </template>

    <template v-if="service">
      <ul v-if="showDeployments(service)" class="space-y-1 text-xs text-ink-muted">
        <li v-for="d in service.deployments" :key="d.id" class="infra-deployment">
          <span class="font-medium text-ink">{{ d.status === 'PRIMARY' ? 'New' : 'Old' }}</span>
          {{ revisionLabel(d.task_definition) ?? d.task_definition }}: {{ d.running }} of {{ d.desired }} running<template
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
            <span :class="badgeClass(healthLabel(t.health).tone)">{{ healthLabel(t.health).label }}</span>
            <span v-if="t.last_status !== 'RUNNING'" class="text-xs text-warn">{{ taskStatusLabel(t.last_status) }}</span>
            <BuildText v-if="taskBuild(t)" class="text-xs" :build="taskBuild(t)" />
            <span v-if="t.on_release === false" class="text-xs text-warn">{{ revisionLabel(t.task_definition) }}, not the release's</span>
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
      <p v-else class="border-t border-border pt-2 text-sm text-ink-muted">No tasks.</p>

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
        <summary class="text-accent hover:text-ink-bright">Recent events ({{ service.events.length }})</summary>
        <ul class="mt-2 space-y-1 text-ink-muted">
          <li v-for="(e, i) in service.events" :key="i" class="infra-event">
            <RelativeTime v-if="e.at" :value="e.at" :now="now" class="whitespace-nowrap" />: {{ e.message }}
          </li>
        </ul>
      </details>
    </template>

    <slot />

    <p v-if="to" class="border-t border-border pt-2 text-sm">
      <RouterLink :to="to" class="link">Usage and logs<span class="sr-only">: {{ name }}</span></RouterLink>
    </p>
  </article>
</template>

<script setup lang="ts">
// One program on the Infrastructure page: the build its tasks run
// (utils/buildView.ts), its ECS service when AWS was read (state, task
// revision, deployments, tasks, stopped tasks, events), what its tasks use now
// and its tasks gone in 24 hours, opening its own page.
import { computed } from 'vue';
import { RouterLink } from 'vue-router';
import BuildText from './BuildText.vue';
import RelativeTime from './RelativeTime.vue';
import type { InfraRelease, InfraService, InfraTask } from '../types/infra';
import type { ProgramUsage } from '../types/usage';
import { buildOf, programBuilds, releaseImageCommit, revisionLabel, type Build } from '../utils/buildView';
import { countLabel, healthLabel, primaryDeployment, rolloutLabel, serviceState, shortImage, taskSize, taskStatusLabel } from '../utils/infraView';
import { badgeClass } from '../utils/tone';
import { usageLine } from '../utils/usageView';

const props = withDefaults(
  defineProps<{
    name: string;
    // ECS's view; null where it was not read, or runs no service of it.
    service: InfraService | null;
    // Its samples' view; null where none was read (hostd's).
    usage: ProgramUsage | null;
    // The release AWS holds (its images' commits name a task's build where the
    // task has not reported one); null where none was read.
    release?: InfraRelease | null;
    // Whether AWS was read (a missing service is then one ECS lacks).
    cloudRead: boolean;
    now: number;
    // The page it opens.
    to?: string | null;
  }>(),
  { to: null, release: null },
);

const reporting = computed(() => (props.usage?.tasks ?? []).some((t) => t.reporting));
const builds = computed(() => programBuilds(props.name, props.usage, props.service, props.release));
// Counts show where its tasks run more than one build, or one not known.
const many = computed(() => builds.value.builds.length > 1 || builds.value.unknown > 0);

// An ECS task's build: what it reported, else its image's commit.
function taskBuild(t: InfraTask): Build | null {
  const said = props.usage?.tasks.find((u) => u.id === t.id);
  const reported = said ? buildOf(said.one_version, said.one_commit) : null;
  if (reported) return reported;
  const commit = releaseImageCommit(props.release, props.name, t.containers.find((c) => c.name === props.name)?.image ?? null);
  return commit ? { version: null, commit } : null;
}

// The deployments listed while there is more than one, or the one is not done.
function showDeployments(s: InfraService): boolean {
  return s.deployments.length > 1 || (s.deployments.length === 1 && s.deployments[0].rollout_state !== 'COMPLETED');
}

function stoppedReason(t: InfraTask): string {
  const exits = t.containers
    .filter((c) => c.exit_code !== null)
    .map((c) => `${c.name} exited with code ${c.exit_code}`);
  return [t.stopped_reason || 'No reason given', ...exits].join('; ');
}
</script>
