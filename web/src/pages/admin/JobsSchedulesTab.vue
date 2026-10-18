<template>
  <section class="space-y-4" aria-labelledby="jobs-schedules">
    <h2 id="jobs-schedules" class="text-xl font-semibold text-ink">Schedules</h2>
    <p v-if="overview" class="jobs-fleet text-sm text-ink-muted">
      <template v-if="overview.fleet.leader">
        The fleet leader <span class="font-mono">{{ overview.fleet.leader }}</span> enqueues them<template
          v-if="overview.fleet.elected_at"
        >, leading since <RelativeTime :value="overview.fleet.elected_at" :now="now" /></template>.
      </template>
      <template v-else>No fleet process has led yet.</template>
      <template v-if="overview.last_scheduled_at">
        The last scheduled job came in <RelativeTime :value="overview.last_scheduled_at" :now="now" />.
      </template>
    </p>
    <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
      <table class="min-w-full divide-y divide-border">
        <caption class="sr-only">Periodic jobs</caption>
        <thead>
          <tr :class="headRow">
            <th scope="col" class="px-4 py-3">Kind</th>
            <th scope="col" class="px-4 py-3">How often</th>
            <th scope="col" class="px-4 py-3">Last run</th>
            <th scope="col" class="px-4 py-3">Next</th>
            <th scope="col" class="px-4 py-3 text-right">Actions</th>
          </tr>
        </thead>
        <tbody class="divide-y divide-border text-sm text-ink">
          <tr v-if="overview === null">
            <td colspan="5" class="px-4 py-6 text-center text-ink-muted">Not loaded.</td>
          </tr>
          <tr v-else-if="overview.schedules.length === 0">
            <td colspan="5" class="px-4 py-6 text-center text-ink-muted">This service schedules no job.</td>
          </tr>
          <tr v-for="s in overview?.schedules ?? []" :key="s.kind" class="jobs-schedule-row align-top hover:bg-hover">
            <th scope="row" class="px-4 py-3 text-left font-normal">
              {{ jobKindLabel(s.kind) }}
              <span class="block font-mono text-xs text-ink-muted">{{ s.kind }}</span>
            </th>
            <td class="px-4 py-3">
              {{ capitalized(everyLabel(s.interval_seconds)) }}
              <span v-if="s.conditional" class="block text-xs text-ink-muted">only while there is work for it</span>
              <span v-if="s.at_start" class="block text-xs text-ink-muted">and when a fleet leader starts</span>
            </td>
            <td class="px-4 py-3">
              <template v-if="s.last">
                <RouterLink :to="`/admin/jobs/${s.last.id}`" :class="jobStateBadgeClass(s.last)">{{ jobStateLabel(s.last) }}</RouterLink>
                <span class="ml-2 text-xs text-ink-muted">
                  <RelativeTime :value="s.last.finished_at ?? s.last.created_at ?? ''" :now="now" /><template v-if="s.last.manual">, run by an admin</template>
                </span>
                <span v-if="s.last_finished && s.last_finished.state === 'failed'" class="block max-w-xs break-words text-xs text-danger">
                  {{ s.last_finished.last_error }}
                </span>
              </template>
              <span v-else class="text-ink-muted">Never</span>
            </td>
            <td class="whitespace-nowrap px-4 py-3">{{ scheduleNextLabel(s, now) }}</td>
            <td class="whitespace-nowrap px-4 py-3 text-right">
              <button
                type="button"
                :disabled="busy || s.pending"
                :title="s.pending ? 'One of this kind is waiting or running' : undefined"
                class="text-accent hover:text-ink-bright disabled:opacity-40"
                @click="runNow(s)"
              >
                Run now<span class="sr-only"> {{ s.kind }}</span>
              </button>
            </td>
          </tr>
        </tbody>
      </table>
    </div>
  </section>
</template>

<script setup lang="ts">
// The background jobs page's Schedules tab (JobsSection.vue): the fleet leader
// that enqueues them, and each periodic kind (how often, its last run and its
// result, the next, run now: POST /api/admin/jobs/run, asked first).
import { ref } from 'vue';
import { RouterLink } from 'vue-router';
import RelativeTime from '../../components/RelativeTime.vue';
import { runJobNow } from '../../api/jobs';
import type { JobSchedule, JobsOverview } from '../../types/jobs';
import { everyLabel, jobKindLabel, jobStateBadgeClass, jobStateLabel, scheduleNextLabel } from '../../utils/jobsView';

defineProps<{
  overview: JobsOverview | null;
  now: number;
}>();

const emit = defineEmits<{
  notice: [notice: { ok: boolean; text: string }];
  changed: [];
}>();

const headRow = 'bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted';

const busy = ref(false);

function capitalized(text: string): string {
  return text.charAt(0).toUpperCase() + text.slice(1);
}

async function runNow(s: JobSchedule) {
  if (busy.value) return;
  if (!window.confirm(`Run "${jobKindLabel(s.kind)}" now? A job of this kind is queued at once, beside its schedule; a worker takes it within seconds.`)) return;
  busy.value = true;
  const result = await runJobNow(s.kind);
  busy.value = false;
  emit(
    'notice',
    result.ok
      ? { ok: true, text: `"${jobKindLabel(s.kind)}" is queued to run now (job ${result.value.id}).` }
      : { ok: false, text: result.error.message },
  );
  emit('changed');
}
</script>
