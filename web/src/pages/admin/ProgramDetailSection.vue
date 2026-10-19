<template>
  <div class="space-y-8">
    <AdminCrumbs />

    <div v-if="programs === null && !loadError" class="animate-pulse rounded-panel border border-border bg-panel p-8 text-center text-ink-muted">
      Loading the program…
    </div>
    <div v-else-if="program === null" class="space-y-3 rounded-panel border border-border bg-panel p-6">
      <h1 class="text-xl font-semibold text-ink">{{ programs ? 'No such program' : 'The program could not be loaded' }}</h1>
      <p role="alert" class="text-sm text-ink-muted">
        {{ programs ? `The service runs no program called ${name}: it runs ${programs.programs.map((p) => p.name).join(', ')}.` : loadError }}
      </p>
      <button v-if="!programs" type="button" :class="secondaryButton" @click="poll.refresh">Try again</button>
    </div>

    <template v-else>
      <header class="flex flex-col gap-3 sm:flex-row sm:items-start sm:justify-between">
        <div class="min-w-0 space-y-1">
          <h1 class="text-3xl font-semibold text-ink">{{ program.name }}</h1>
          <p class="text-sm text-ink-muted">
            <template v-if="program.desired !== null">{{ program.running }} of {{ program.desired }} tasks running</template>
            <template v-else>{{ countLabel(program.running, 'task') }} reporting</template>
            <template v-if="program.release">, revision <span class="font-mono text-xs">{{ program.release }}</span></template>
            <span v-if="program.on_release === false" class="text-warn">, not the release's</span>
          </p>
          <p class="program-now text-sm" :class="reporting ? 'text-ink' : 'text-warn'">{{ usageLine(program.name, reporting ? program : null) }}</p>
          <p class="program-restarts text-xs" :class="program.restarts_24h > 0 ? 'font-semibold text-warn' : 'text-ink-muted'">
            {{
              program.restarts_24h === 0
                ? 'No task gone in the last 24 hours.'
                : `${countLabel(program.restarts_24h, 'task')} gone in the last 24 hours (a crash, a deploy or a scale-in).`
            }}
          </p>
        </div>
        <div class="flex items-center gap-3">
          <p v-if="loadedAt" class="text-xs text-ink-muted">Updated <RelativeTime :value="loadedAt" :now="now" /></p>
          <button type="button" :class="secondaryButton" :disabled="poll.loading.value" @click="poll.refresh">
            {{ poll.loading.value ? 'Refreshing…' : 'Refresh' }}
          </button>
        </div>
      </header>
      <p v-if="pollTrouble" class="text-xs text-warn">The last refresh did not get through; this page keeps trying.</p>
      <p v-if="programs?.cloud.unavailable" class="text-xs text-ink-muted">{{ programs.cloud.unavailable }}: the tasks below are the ones reporting.</p>

      <!-- Its tasks, each what it uses now. -->
      <section class="space-y-3" aria-labelledby="program-tasks">
        <h2 id="program-tasks" class="text-xl font-semibold text-ink">Tasks</h2>
        <p v-if="program.tasks.length === 0" class="text-sm text-ink-muted">No task runs or reports.</p>
        <ul v-else class="divide-y divide-border rounded-panel border border-border bg-panel text-sm">
          <li v-for="t in program.tasks" :key="t.id" class="program-task space-y-1 px-4 py-2">
            <p class="flex flex-wrap items-center gap-x-3 gap-y-1">
              <span class="font-mono text-ink" :title="`Task ${t.id}`">{{ shortTask(t.id) }}</span>
              <span :class="toneBadgeClass(t.reporting ? 'ok' : 'warn')">{{ t.reporting ? 'Reporting' : 'Not reporting' }}</span>
              <span v-if="t.health" :class="toneBadgeClass(healthLabel(t.health).tone)">{{ healthLabel(t.health).label }}</span>
              <span v-if="t.last_status && t.last_status !== 'RUNNING'" class="text-xs text-warn">{{ taskStatusLabel(t.last_status) }}</span>
            </p>
            <p class="text-xs" :class="t.reporting ? 'text-ink' : 'text-ink-muted'">
              {{ t.reporting ? usageLine(`Task ${shortTask(t.id)}`, t) : 'It has sent no sample in the last 3 minutes.' }}
            </p>
            <p v-if="t.started_at || t.task_definition" class="text-xs text-ink-muted">
              <template v-if="t.started_at">Started <RelativeTime :value="t.started_at" :now="now" /></template><template
                v-if="t.started_at && t.task_definition"
                >, </template
              ><span v-if="t.task_definition" class="font-mono">{{ t.task_definition }}</span>
            </p>
          </li>
        </ul>
      </section>

      <!-- Its usage and its logs, all its tasks or one. -->
      <div class="space-y-4">
        <div class="flex flex-col gap-3 sm:flex-row sm:items-end sm:justify-between">
          <nav aria-label="The program's views" class="border-b border-border">
            <ul class="-mb-px flex gap-1">
              <li v-for="t in TABS" :key="t.key">
                <RouterLink
                  :to="t.key === 'usage' ? { path: base } : { path: base, query: { tab: t.key } }"
                  class="block whitespace-nowrap border-b-2 px-3 py-2 text-sm font-medium transition"
                  :class="tab === t.key ? 'border-accent text-ink' : 'border-transparent text-ink-muted hover:text-ink'"
                  :aria-current="tab === t.key ? 'page' : undefined"
                >
                  {{ t.label }}
                </RouterLink>
              </li>
            </ul>
          </nav>
          <label class="block max-w-xs space-y-1">
            <span class="block text-xs font-medium text-ink-muted">Tasks</span>
            <select
              id="program-task"
              v-model="task"
              class="w-full rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none"
            >
              <option value="">All its tasks together</option>
              <option v-for="t in program.tasks" :key="t.id" :value="t.id">Task {{ shortTask(t.id) }}{{ t.reporting ? '' : ' (not reporting)' }}</option>
            </select>
          </label>
        </div>

        <UsageCharts
          v-if="tab === 'usage'"
          id-base="program-usage"
          subject="program"
          :id="program.name"
          :task="task || undefined"
          :name="task ? `${program.name} task ${shortTask(task)}` : program.name"
          :heading="task ? `Task ${shortTask(task)}'s usage` : 'Usage, all its tasks together'"
          :heading-level="2"
        />
        <LogViewer
          v-else
          id-base="program-log"
          subject="program"
          :id="program.name"
          :task="task || undefined"
          :heading="task ? `Task ${shortTask(task)}'s log` : 'Its log, every task'"
          :heading-level="2"
        />
      </div>
    </template>
  </div>
</template>

<script setup lang="ts">
// /admin/infrastructure/programs/<name>: one of the service's programs (GET
// /api/admin/programs): its tasks running and desired, its revision, what it
// uses now and its tasks gone in 24 hours; each task with what it uses; its
// usage (UsageCharts, all its tasks together or one) and, on the Logs tab
// (?tab=logs), its log over its tasks' streams or one's (LogViewer). Polled
// every 30 s.
import AdminCrumbs from '../../components/AdminCrumbs.vue';
import { computed, ref, watch } from 'vue';
import { RouterLink, useRoute } from 'vue-router';
import LogViewer from '../../components/LogViewer.vue';
import RelativeTime from '../../components/RelativeTime.vue';
import UsageCharts from '../../components/UsageCharts.vue';
import { fetchPrograms } from '../../api/usage';
import { usePolling } from '../../composables/usePolling';
import type { ProgramsResponse } from '../../types/usage';
import { countLabel, healthLabel, taskStatusLabel, toneBadgeClass } from '../../utils/infraView';
import { usageLine } from '../../utils/usageView';

const POLL_MS = 30_000;
const TABS = [
  { key: 'usage', label: 'Usage' },
  { key: 'logs', label: 'Logs' },
] as const;

const secondaryButton =
  'rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40';

const route = useRoute();
const name = computed(() => String(route.params.name ?? ''));
const base = computed(() => `/admin/infrastructure/programs/${name.value}`);
const tab = computed(() => (route.query.tab === 'logs' ? 'logs' : 'usage'));

const programs = ref<ProgramsResponse | null>(null);
const loadError = ref('');
const pollTrouble = ref(false);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
// The task picked ('' for all of them together).
const task = ref('');

const program = computed(() => programs.value?.programs.find((p) => p.name === name.value) ?? null);
const reporting = computed(() => (program.value?.tasks ?? []).some((t) => t.reporting));

// An ECS task id's first 12 digits; a local container's id the same.
function shortTask(id: string): string {
  return id.slice(0, 12);
}

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
