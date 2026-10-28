<template>
  <div class="space-y-8">
    <section class="space-y-3" aria-labelledby="jobs-attention">
      <h2 id="jobs-attention" class="section-heading">Needs attention</h2>
      <div class="table-wrap">
        <table class="data-table">
          <thead>
            <tr>
              <th scope="col">Kind</th>
              <th scope="col">Why</th>
              <th scope="col">How many</th>
              <th scope="col">Last</th>
              <th scope="col"><span class="sr-only">Actions</span></th>
            </tr>
          </thead>
          <tbody>
            <TableState
              v-if="!overview || overview.attention.length === 0"
              :colspan="5"
              :rows="overview?.attention ?? null"
              :loading="loading"
              empty="No job failed today."
            />
            <tr v-for="g in overview?.attention ?? []" :key="`${g.kind} ${g.key}`" class="jobs-attention-row">
              <th scope="row" class="font-normal" :title="g.kind">{{ jobKindLabel(g.kind) }}</th>
              <td class="max-w-md break-words text-danger">{{ failureText(g) }}</td>
              <td class="whitespace-nowrap">
                {{ formatCount(g.count) }} job{{ g.count === 1 ? '' : 's' }}
                <span v-if="g.retryable < g.count" class="block text-xs text-ink-muted">
                  {{ g.retryable === 0 ? 'none' : formatCount(g.retryable) }} can run again
                </span>
              </td>
              <td class="whitespace-nowrap text-xs text-ink-muted">
                <RelativeTime v-if="g.last_at" :value="g.last_at" :now="now" />
                <span class="block"><JobLink :id="g.last_job_id" /></span>
              </td>
              <td class="whitespace-nowrap text-right">
                <RouterLink :to="jobsOf(g)" class="btn-ghost btn-sm">Show<span class="sr-only"> the {{ g.kind }} jobs that failed this way</span></RouterLink>
                <button v-if="g.retryable > 0" type="button" class="btn-ghost btn-sm" :disabled="busy" @click="retryAll(g)">
                  Retry all<span class="sr-only">{{ ' ' + (g.kind) }} jobs that failed this way</span>
                </button>
                <button type="button" class="btn-ghost btn-sm" :disabled="busy" @click="dismiss(g)">
                  Dismiss<span class="sr-only"> the {{ g.kind }} jobs that failed this way</span>
                </button>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
      <p v-if="overview && hiddenFailures > 0" class="jobs-attention-more text-xs text-ink-muted">
        <RouterLink :to="{ path: '/admin/system/jobs', query: { tab: 'jobs', state: 'failed' } }" class="link">
          {{ formatCount(hiddenFailures) }} more failed job{{ hiddenFailures === 1 ? '' : 's' }}
        </RouterLink>
      </p>
    </section>

    <section class="space-y-3" aria-labelledby="jobs-workers">
      <h2 id="jobs-workers" class="section-heading">Workers</h2>
      <div class="table-wrap">
        <table class="data-table">
          <thead>
            <tr>
              <th scope="col">Process</th>
              <th scope="col">Threads</th>
              <th scope="col">Leases</th>
              <th scope="col">Last hour</th>
              <th scope="col">Last active</th>
            </tr>
          </thead>
          <tbody>
            <TableState
              v-if="!overview || overview.workers.length === 0"
              :colspan="5"
              :rows="overview?.workers ?? null"
              :loading="loading"
              empty="No worker in the last hour."
            />
            <tr v-for="w in overview?.workers ?? []" :key="w.process" class="jobs-worker-row">
              <th scope="row" class="whitespace-nowrap font-mono text-xs font-normal">{{ w.process }}</th>
              <td class="whitespace-nowrap">
                <RouterLink v-if="w.busy > 0" :to="{ path: '/admin/system/jobs', query: { tab: 'jobs', state: 'running' } }" class="entity-link">
                  {{ w.busy }} of {{ w.threads }} busy
                </RouterLink>
                <template v-else>{{ w.busy }} of {{ w.threads }} busy</template>
              </td>
              <td :class="w.lapsed > 0 ? 'font-semibold text-danger' : 'text-ink-muted'">{{ workerLeaseLabel(w) || 'None' }}</td>
              <td class="whitespace-nowrap">
                {{ formatCount(w.done) }} done<span v-if="w.failed" class="text-danger">, {{ formatCount(w.failed) }} failed</span>
              </td>
              <td class="whitespace-nowrap text-xs text-ink-muted">
                <RelativeTime v-if="w.last_active_at" :value="w.last_active_at" :now="now" />
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </section>

    <section class="space-y-3" aria-labelledby="jobs-kinds">
      <h2 id="jobs-kinds" class="section-heading">By kind</h2>
      <div class="table-wrap">
        <table class="data-table">
          <thead>
            <tr>
              <th scope="col">Kind</th>
              <th scope="col">Waiting</th>
              <th scope="col">Running</th>
              <th scope="col">Done, hour</th>
              <th scope="col">Failed, day</th>
              <th scope="col">Oldest wait</th>
              <th scope="col">Mean run</th>
            </tr>
          </thead>
          <tbody>
            <TableState
              v-if="!overview || overview.kinds.length === 0"
              :colspan="7"
              :rows="overview?.kinds ?? null"
              :loading="loading"
              empty="No jobs yet."
            />
            <tr v-for="k in overview?.kinds ?? []" :key="k.kind" class="jobs-kind-row">
              <th scope="row" class="font-normal">
                <RouterLink :to="{ path: '/admin/system/jobs', query: { tab: 'jobs', kind: k.kind } }" class="entity-link" :title="k.kind">{{ jobKindLabel(k.kind) }}</RouterLink>
              </th>
              <td class="whitespace-nowrap">{{ k.due }} due<template v-if="k.queued > k.due">, {{ k.queued - k.due }} later</template></td>
              <td class="whitespace-nowrap">{{ k.running }}</td>
              <td class="whitespace-nowrap">{{ formatCount(k.done_hour) }}</td>
              <td class="whitespace-nowrap" :class="k.failed_day > 0 ? 'text-danger' : ''">{{ formatCount(k.failed_day) }}</td>
              <td class="whitespace-nowrap">{{ k.oldest_wait_seconds === null ? 'None' : secondsLabel(k.oldest_wait_seconds) }}</td>
              <td class="whitespace-nowrap">{{ k.mean_run_seconds === null ? 'None' : secondsLabel(k.mean_run_seconds) }}</td>
            </tr>
          </tbody>
        </table>
      </div>
    </section>
  </div>
</template>

<script setup lang="ts">
// The jobs page's Overview: today's failures, alike ones together (each to its
// jobs, retried or dismissed), the worker processes, and each kind's counts.
import { computed, ref } from 'vue';
import { RouterLink } from 'vue-router';
import RelativeTime from '../../../components/RelativeTime.vue';
import JobLink from '../../../components/links/JobLink.vue';
import TableState from '../../../components/ui/TableState.vue';
import { dismissJobGroup, retryJobGroup } from '../../../api/jobs';
import { confirmAction } from '../../../composables/useConfirm';
import type { JobFailureGroup, JobsOverview } from '../../../types/jobs';
import { formatCount } from '../../../utils/format';
import { groupRetryMessage, jobKindLabel, secondsLabel, workerLeaseLabel } from '../../../utils/jobsView';

const props = defineProps<{
  overview: JobsOverview | null;
  loading: boolean;
  now: number;
}>();

const emit = defineEmits<{
  notice: [notice: { ok: boolean; text: string }];
  changed: [];
}>();

const busy = ref(false);

// The failed jobs of the groups the overview leaves out (past its newest).
const hiddenFailures = computed(() => {
  const o = props.overview;
  if (!o) return 0;
  return o.attention_total - o.attention.reduce((n, g) => n + g.count, 0);
});

// A lone failure's own error; alike ones' shared words.
function failureText(g: JobFailureGroup): string {
  const text = g.count === 1 ? g.last_error : g.key;
  return text || 'No reason recorded';
}

// The Jobs view filtered to the group's jobs.
function jobsOf(g: JobFailureGroup) {
  return { path: '/admin/system/jobs', query: { tab: 'jobs', state: 'failed', kind: g.kind, failure: g.key } };
}

async function dismiss(g: JobFailureGroup) {
  if (busy.value) return;
  if (!await confirmAction(`Dismiss ${g.count === 1 ? 'this failed job' : `these ${g.count} failed jobs`}? They are deleted.`)) return;
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
    n === 1 ? `Retry the failed "${jobKindLabel(g.kind)}" job now? It gets one more attempt.` : `Retry ${n} failed "${jobKindLabel(g.kind)}" jobs now? Each gets one more attempt.`;
  if (!await confirmAction(question)) return;
  busy.value = true;
  const result = await retryJobGroup(g.kind, g.key);
  busy.value = false;
  emit('notice', result.ok ? { ok: true, text: groupRetryMessage(result.value) } : { ok: false, text: result.error.message });
  emit('changed');
}
</script>
