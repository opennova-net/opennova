<template>
  <div class="space-y-8">
    <PageHeader :title="program?.name ?? name" :crumbs="CRUMBS" refreshable :loaded-at="loadedAt" :now="now" :loading="poll.loading.value" @refresh="poll.refresh">
      <template v-if="program" #meta>
        <span v-if="program.desired !== null">{{ program.running }} of {{ program.desired }} tasks running</span>
        <span v-else>{{ countLabel(program.running, 'task') }} reporting</span>
        <span v-for="b in builds.builds" :key="`${b.build.version} ${b.build.commit}`" class="program-build">
          <BuildText :build="b.build" /><template v-if="builds.builds.length > 1 || builds.unknown"> ({{ countLabel(b.count, 'task') }})</template>
        </span>
        <span v-if="builds.unknown" class="program-build-unknown text-xs text-warn">Build not known for {{ countLabel(builds.unknown, 'task') }}</span>
        <span v-if="program.task_definition" class="program-revision text-xs">{{ revisionLabel(program.task_definition) ?? program.task_definition }}</span>
        <Badge v-if="program.on_release === false" tone="warn">Not the release's</Badge>
        <Badge v-if="program.restarts_24h > 0" tone="warn" class="program-restarts">{{ countLabel(program.restarts_24h, 'task') }} gone in 24 hours</Badge>
      </template>
    </PageHeader>

    <LiveNotices :poll-trouble="pollTrouble" />
    <p v-if="programs === null && !loadError" class="text-sm text-ink-muted">Loading…</p>
    <div v-else-if="program === null" class="space-y-3">
      <Notice tone="bad" role="alert">{{ programs ? `No program is called ${name}.` : loadError }}</Notice>
      <button v-if="!programs" type="button" class="btn-secondary" @click="poll.refresh">Try again</button>
    </div>

    <template v-else>
      <p v-if="programs?.cloud.unavailable" class="text-xs text-ink-muted" :title="programs.cloud.unavailable">AWS is not read on this stack: the tasks are the ones reporting.</p>
      <p class="program-now text-sm" :class="reporting ? 'text-ink' : 'text-warn'">{{ usageLine(program.name, reporting ? program : null) }}</p>

      <section class="space-y-3" aria-labelledby="program-tasks">
        <h2 id="program-tasks" class="section-heading">Tasks</h2>
        <div class="table-wrap">
          <table class="data-table">
            <thead>
              <tr>
                <th scope="col">Task</th>
                <th scope="col">Build</th>
                <th scope="col">State</th>
                <th scope="col">Uses now</th>
                <th scope="col">Started</th>
              </tr>
            </thead>
            <tbody>
              <TableState v-if="program.tasks.length === 0" :colspan="5" :rows="program.tasks" empty="No tasks." />
              <tr v-for="t in program.tasks" :key="t.id" class="program-task">
                <th scope="row" class="whitespace-nowrap font-mono text-xs font-normal" :title="`Task ${t.id}`">
                  {{ shortTaskId(t.id) }}
                  <span v-if="t.task_definition" class="block font-sans text-ink-muted">{{ revisionLabel(t.task_definition) ?? t.task_definition }}</span>
                </th>
                <td class="whitespace-nowrap text-xs"><BuildText :build="buildOf(t.one_version, t.one_commit)" /></td>
                <td class="whitespace-nowrap">
                  <Badge :tone="t.reporting ? 'ok' : 'warn'">{{ t.reporting ? 'Reporting' : 'Not reporting' }}</Badge>
                  <Badge v-if="t.health" class="ml-1" :tone="healthLabel(t.health).tone">{{ healthLabel(t.health).label }}</Badge>
                  <span v-if="t.last_status && t.last_status !== 'RUNNING'" class="ml-1 text-xs text-warn">{{ taskStatusLabel(t.last_status) }}</span>
                </td>
                <td class="text-xs" :class="t.reporting ? 'text-ink' : 'text-ink-muted'">{{ t.reporting ? usageLine(`Task ${shortTaskId(t.id)}`, t) : 'No sample in 3 minutes' }}</td>
                <td class="whitespace-nowrap text-xs text-ink-muted"><RelativeTime v-if="t.started_at" :value="t.started_at" :now="now" /></td>
              </tr>
            </tbody>
          </table>
        </div>
      </section>

      <div class="space-y-4">
        <div class="flex flex-col gap-3 sm:flex-row sm:items-end sm:justify-between">
          <TabNav :tabs="tabs" label="The program's views" class="min-w-0 flex-1" />
          <label class="flex items-center gap-2 text-sm">
            <span class="text-ink-muted">Tasks</span>
            <select id="program-task" v-model="task" class="field w-auto">
              <option value="">All together</option>
              <option v-for="t in program.tasks" :key="t.id" :value="t.id">Task {{ shortTaskId(t.id) }}{{ t.reporting ? '' : ' (not reporting)' }}</option>
            </select>
          </label>
        </div>

        <UsageCharts
          v-if="tab === 'usage'"
          id-base="program-usage"
          subject="program"
          :id="program.name"
          :task="task || undefined"
          :name="task ? `${program.name} task ${shortTaskId(task)}` : program.name"
          :heading="task ? `Task ${shortTaskId(task)}'s usage` : 'Usage, all tasks'"
          :heading-level="2"
        />
        <LogViewer
          v-else
          id-base="program-log"
          subject="program"
          :id="program.name"
          :task="task || undefined"
          :heading="task ? `Task ${shortTaskId(task)}'s log` : 'Log, all tasks'"
          :heading-level="2"
        />
      </div>
    </template>
  </div>
</template>

<script setup lang="ts">
// /admin/programs/<name>: one of the service's programs: the build its tasks
// say they run, its tasks (each with its build) and what each uses now, then its
// usage (all its tasks or one) and, on ?tab=logs, its log. ?task= picks a task
// (a lobby's, from the NovaWorld tab). Polled every 30 s.
import { computed, ref, watch } from 'vue';
import { useRoute } from 'vue-router';
import BuildText from '../../../components/BuildText.vue';
import LogViewer from '../../../components/LogViewer.vue';
import RelativeTime from '../../../components/RelativeTime.vue';
import UsageCharts from '../../../components/UsageCharts.vue';
import Badge from '../../../components/ui/Badge.vue';
import LiveNotices from '../../../components/ui/LiveNotices.vue';
import Notice from '../../../components/ui/Notice.vue';
import PageHeader from '../../../components/ui/PageHeader.vue';
import TabNav, { type Tab } from '../../../components/ui/TabNav.vue';
import TableState from '../../../components/ui/TableState.vue';
import { fetchPrograms } from '../../../api/usage';
import { usePolling } from '../../../composables/usePolling';
import type { ProgramsResponse } from '../../../types/usage';
import { shortTaskId } from '../../../utils/adminView';
import type { Crumb } from '../../../utils/adminNav';
import { buildOf, programBuilds, revisionLabel } from '../../../utils/buildView';
import { countLabel, healthLabel, taskStatusLabel } from '../../../utils/infraView';
import { usageLine } from '../../../utils/usageView';

const POLL_MS = 30_000;
const CRUMBS: Crumb[] = [
  { label: 'System', to: '/admin/system' },
  { label: 'Infrastructure', to: '/admin/system/infrastructure' },
];

const route = useRoute();
const name = computed(() => String(route.params.name ?? ''));
const base = computed(() => `/admin/programs/${name.value}`);
const tab = computed(() => (route.query.tab === 'logs' ? 'logs' : 'usage'));
const tabs = computed<Tab[]>(() => [
  { label: 'Usage', to: { path: base.value } },
  { label: 'Logs', to: { path: base.value, query: { tab: 'logs' } } },
]);

const programs = ref<ProgramsResponse | null>(null);
const loadError = ref('');
const pollTrouble = ref(false);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
// The task picked ('' for all of them together): first the one a link names.
const task = ref(typeof route.query.task === 'string' ? route.query.task : '');

const program = computed(() => programs.value?.programs.find((p) => p.name === name.value) ?? null);
const reporting = computed(() => (program.value?.tasks ?? []).some((t) => t.reporting));
// The builds its tasks say they run (this page reads no AWS, so no image's).
const builds = computed(() => programBuilds(name.value, program.value, null, null));

const poll = usePolling({
  async read(signal, background) {
    const result = await fetchPrograms(signal);
    if (signal.aborted) return false;
    now.value = Date.now();
    if (result.ok) {
      programs.value = result.value;
      loadedAt.value = now.value;
      loadError.value = '';
      pollTrouble.value = false;
      return true;
    }
    if (background && programs.value !== null) pollTrouble.value = true;
    else loadError.value = result.error.message;
    return false;
  },
  next: () => POLL_MS,
});

// Another program in the same place: all its tasks again.
watch(name, () => {
  task.value = '';
});
</script>
