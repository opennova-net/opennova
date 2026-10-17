<template>
  <div class="space-y-10">
    <!-- The failures, alike ones together. -->
    <section class="space-y-4" aria-labelledby="jobs-attention">
      <h2 id="jobs-attention" class="text-xl font-semibold text-ink">Needs attention</h2>
      <p class="text-sm text-ink-muted">
        Jobs that failed in the last day, their attempts used up or failed for good (an admin's cancels left out), alike
        failures together: the same kind, and the same error once its ids, numbers and times are set aside. A retry queues
        each of them again to run now with one more attempt.
      </p>
      <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
        <table class="min-w-full divide-y divide-border">
          <caption class="sr-only">Failed jobs, alike failures together</caption>
          <thead>
            <tr :class="headRow">
              <th scope="col" class="px-4 py-3">Kind</th>
              <th scope="col" class="px-4 py-3">Why</th>
              <th scope="col" class="px-4 py-3">How many</th>
              <th scope="col" class="px-4 py-3">Last</th>
              <th scope="col" class="px-4 py-3 text-right">Actions</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border text-sm text-ink">
            <tr v-if="overview === null">
              <td colspan="5" class="px-4 py-6 text-center text-ink-muted">{{ loading ? 'Loading…' : 'Not loaded.' }}</td>
            </tr>
            <tr v-else-if="overview.attention.length === 0">
              <td colspan="5" class="px-4 py-6 text-center text-ink-muted">No job failed in the last day.</td>
            </tr>
            <tr v-for="g in overview?.attention ?? []" :key="`${g.kind} ${g.key}`" class="jobs-attention-row align-top hover:bg-hover">
              <th scope="row" class="px-4 py-3 text-left font-normal">
                {{ jobKindLabel(g.kind) }}
                <span class="block font-mono text-xs text-ink-muted">{{ g.kind }}</span>
              </th>
              <td class="max-w-md break-words px-4 py-3 text-danger">{{ failureText(g) }}</td>
              <td class="whitespace-nowrap px-4 py-3">
                {{ formatCount(g.count) }} job{{ g.count === 1 ? '' : 's' }}
                <span v-if="g.retryable < g.count" class="block text-xs text-ink-muted">
                  {{ g.retryable === 0 ? 'none' : formatCount(g.retryable) }} can run again
                </span>
              </td>
              <td class="whitespace-nowrap px-4 py-3 text-xs text-ink-muted">
                <RelativeTime v-if="g.last_at" :value="g.last_at" :now="now" />
                <RouterLink :to="`/admin/jobs/${g.last_job_id}`" class="block text-accent hover:text-ink-bright">job {{ g.last_job_id }}</RouterLink>
              </td>
              <td class="space-x-3 whitespace-nowrap px-4 py-3 text-right">
                <RouterLink :to="jobsOf(g)" class="text-accent hover:text-ink-bright">
                  Show the jobs<span class="sr-only"> of {{ g.kind }} that failed this way</span>
                </RouterLink>
                <button
                  v-if="g.retryable > 0"
                  type="button"
                  :disabled="busy"
                  class="text-accent hover:text-ink-bright disabled:opacity-40"
                  @click="retryAll(g)"
                >
                  Retry all<span class="sr-only"> {{ g.kind }} jobs that failed this way</span>
                </button>
                <button type="button" :disabled="busy" class="text-ink-muted hover:text-ink disabled:opacity-40" @click="dismiss(g)">
                  Dismiss<span class="sr-only"> the {{ g.kind }} jobs that failed this way</span>
                </button>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
      <p v-if="overview && hiddenFailures > 0" class="jobs-attention-more text-xs text-ink-muted">
        {{ formatCount(hiddenFailures) }} more failed job{{ hiddenFailures === 1 ? '' : 's' }} of older kinds of failure are
        not shown; the Jobs tab lists every one.
      </p>
    </section>

    <!-- The worker processes. -->
    <section class="space-y-4" aria-labelledby="jobs-workers">
      <h2 id="jobs-workers" class="text-xl font-semibold text-ink">Workers</h2>
      <p class="text-sm text-ink-muted">
        A worker holds each job under a lease of a minute that it renews while the job runs. A lease that ran out means its
        process went away; the keeper queues the job again. A process shows once one of its threads has taken a job in the
        last hour.
      </p>
      <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
        <table class="min-w-full divide-y divide-border">
          <caption class="sr-only">Worker processes seen in the last hour</caption>
          <thead>
            <tr :class="headRow">
              <th scope="col" class="px-4 py-3">Worker process</th>
              <th scope="col" class="px-4 py-3">Threads</th>
              <th scope="col" class="px-4 py-3">Leases</th>
              <th scope="col" class="px-4 py-3">Last hour</th>
              <th scope="col" class="px-4 py-3">Last active</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border text-sm text-ink">
            <tr v-if="overview !== null && overview.workers.length === 0">
              <td colspan="5" class="px-4 py-6 text-center text-ink-muted">No worker has taken a job in the last hour.</td>
            </tr>
            <tr v-for="w in overview?.workers ?? []" :key="w.process" class="jobs-worker-row align-top hover:bg-hover">
              <th scope="row" class="whitespace-nowrap px-4 py-3 text-left font-mono text-xs font-normal">{{ w.process }}</th>
              <td class="whitespace-nowrap px-4 py-3">
                {{ w.busy }} of {{ w.threads }} busy
                <RouterLink v-if="w.busy > 0" :to="{ path: '/admin/jobs', query: { tab: 'jobs', state: 'running' } }" class="block text-xs text-accent hover:text-ink-bright">
                  Running jobs
                </RouterLink>
              </td>
              <td class="px-4 py-3" :class="w.lapsed > 0 ? 'font-semibold text-danger' : 'text-ink-muted'">
                {{ workerLeaseLabel(w) || 'None held' }}
              </td>
              <td class="whitespace-nowrap px-4 py-3">
                {{ formatCount(w.done) }} done<span v-if="w.failed" class="text-danger">, {{ formatCount(w.failed) }} failed</span>
              </td>
              <td class="whitespace-nowrap px-4 py-3 text-xs text-ink-muted">
                <RelativeTime v-if="w.last_active_at" :value="w.last_active_at" :now="now" />
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </section>

    <!-- Each kind's counts. -->
    <section class="space-y-4" aria-labelledby="jobs-kinds">
      <h2 id="jobs-kinds" class="text-xl font-semibold text-ink">By kind</h2>
      <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
        <table class="min-w-full divide-y divide-border">
          <caption class="sr-only">Jobs by kind and state</caption>
          <thead>
            <tr :class="headRow">
              <th scope="col" class="px-4 py-2">Kind</th>
              <th scope="col" class="px-4 py-2">Waiting</th>
              <th scope="col" class="px-4 py-2">Running</th>
              <th scope="col" class="px-4 py-2">Done, last hour</th>
              <th scope="col" class="px-4 py-2">Failed, last day</th>
              <th scope="col" class="px-4 py-2">Oldest wait</th>
              <th scope="col" class="px-4 py-2">Mean run time</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border text-sm text-ink">
            <tr v-if="overview !== null && overview.kinds.length === 0">
              <td colspan="7" class="px-4 py-4 text-center text-ink-muted">No job in the queue's history.</td>
            </tr>
            <tr v-for="k in overview?.kinds ?? []" :key="k.kind" class="jobs-kind-row hover:bg-hover">
              <th scope="row" class="px-4 py-2 text-left font-normal" :title="k.kind">{{ jobKindLabel(k.kind) }}</th>
              <td class="whitespace-nowrap px-4 py-2">{{ k.due }} due<template v-if="k.queued > k.due">, {{ k.queued - k.due }} later</template></td>
              <td class="whitespace-nowrap px-4 py-2">{{ k.running }}</td>
              <td class="whitespace-nowrap px-4 py-2">{{ formatCount(k.done_hour) }}</td>
              <td class="whitespace-nowrap px-4 py-2" :class="k.failed_day > 0 ? 'text-danger' : ''">{{ formatCount(k.failed_day) }}</td>
              <td class="whitespace-nowrap px-4 py-2">{{ k.oldest_wait_seconds === null ? 'None waiting' : secondsLabel(k.oldest_wait_seconds) }}</td>
              <td class="whitespace-nowrap px-4 py-2">{{ k.mean_run_seconds === null ? 'None ran' : secondsLabel(k.mean_run_seconds) }}</td>
            </tr>
          </tbody>
        </table>
      </div>
    </section>
  </div>
</template>

<script setup lang="ts">
// The background jobs page's Overview tab (JobsSection.vue): the failures to
// look at, alike ones together (each group to its jobs on the Jobs tab, and a
// retry of every one of them that can run again: POST
// /api/admin/jobs/retry-group, asked first), the worker processes with their
// threads and leases, and each kind's counts.
import { computed, ref } from 'vue';
import { RouterLink } from 'vue-router';
import RelativeTime from '../../components/RelativeTime.vue';
import { dismissJobGroup, retryJobGroup } from '../../api/jobs';
import type { JobFailureGroup, JobsOverview } from '../../types/jobs';
import { formatCount } from '../../utils/format';
import { groupRetryMessage, jobKindLabel, secondsLabel, workerLeaseLabel } from '../../utils/jobsView';

const props = defineProps<{
  overview: JobsOverview | null;
  loading: boolean;
  now: number;
}>();

const emit = defineEmits<{
  notice: [notice: { ok: boolean; text: string }];
  changed: [];
}>();

const headRow = 'bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted';

const busy = ref(false);

// The failed jobs of the groups the overview leaves out (past its newest).
const hiddenFailures = computed(() => {
  const o = props.overview;
  if (!o) return 0;
  return o.attention_total - o.attention.reduce((n, g) => n + g.count, 0);
});

// A lone failure's own error; alike ones' shared words (their ids, numbers
// and times set aside).
function failureText(g: JobFailureGroup): string {
  const text = g.count === 1 ? g.last_error : g.key;
  return text || 'No reason recorded';
}

// The Jobs tab filtered to the group's jobs.
function jobsOf(g: JobFailureGroup) {
  return { path: '/admin/jobs', query: { tab: 'jobs', state: 'failed', kind: g.kind, failure: g.key } };
}

async function dismiss(g: JobFailureGroup) {
  if (busy.value) return;
  if (!window.confirm(`Dismiss ${g.count === 1 ? 'this failed job' : `these ${g.count} failed jobs`}? They are deleted.`)) return;
  busy.value = true;
  const result = await dismissJobGroup(g.kind, g.key);
  busy.value = false;
  emit('notice', result.ok ? { ok: true, text: `${result.value} dismissed.` } : { ok: false, text: result.error.message });
  emit('changed');
}

async function retryAll(g: JobFailureGroup) {
  if (busy.value) return;
  const n = g.retryable;
  const question =
    n === 1
      ? `Retry the failed "${jobKindLabel(g.kind)}" job now? It gets one more attempt.`
      : `Retry the ${n} failed "${jobKindLabel(g.kind)}" jobs that failed this way now? Each gets one more attempt.`;
  if (!window.confirm(question)) return;
  busy.value = true;
  const result = await retryJobGroup(g.kind, g.key);
  busy.value = false;
  emit('notice', result.ok ? { ok: true, text: groupRetryMessage(result.value) } : { ok: false, text: result.error.message });
  emit('changed');
}
</script>
