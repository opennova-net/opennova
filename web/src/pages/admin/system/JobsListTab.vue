<template>
  <section class="space-y-3" aria-label="Jobs">
    <form class="jobs-filter flex flex-wrap items-end gap-2" @submit.prevent="applyFilter">
      <label>
        <span class="field-label">State</span>
        <select v-model="filter.state" name="state" class="field w-auto">
          <option value="">Any</option>
          <option value="queued">Waiting</option>
          <option value="running">Running</option>
          <option value="done">Done</option>
          <option value="failed">Failed or cancelled</option>
        </select>
      </label>
      <label>
        <span class="field-label">Kind</span>
        <select v-model="filter.kind" name="kind" class="field w-auto">
          <option value="">Any</option>
          <option v-for="k in kindChoices" :key="k" :value="k">{{ jobKindLabel(k) }}</option>
        </select>
      </label>
      <label>
        <span class="field-label">From (UTC)</span>
        <input v-model="filter.from" name="from" type="datetime-local" class="field w-auto" />
      </label>
      <label>
        <span class="field-label">Before (UTC)</span>
        <input v-model="filter.to" name="to" type="datetime-local" class="field w-auto" />
      </label>
      <label>
        <span class="field-label">Key</span>
        <input v-model="filter.q" name="q" type="search" autocomplete="off" maxlength="128" placeholder="Contains" class="field w-auto" />
      </label>
      <button type="submit" class="btn-secondary btn-sm">Apply</button>
      <button type="button" class="btn-secondary btn-sm" @click="resetFilter">Reset</button>
    </form>
    <p v-if="query.failure" class="jobs-failure-filter flex flex-wrap items-center gap-x-2 gap-y-1 text-sm">
      <span class="text-ink-muted">Failed with</span>
      <span class="break-all font-mono text-xs text-danger">{{ query.failure }}</span>
      <RouterLink :to="withoutFailure" class="link">Clear</RouterLink>
    </p>
    <p v-if="filterError" role="alert" class="text-sm text-danger">{{ filterError }}</p>

    <div class="table-wrap">
      <table class="data-table">
        <caption class="sr-only">Jobs, newest first</caption>
        <thead>
          <tr>
            <th scope="col">Job</th>
            <th scope="col">State</th>
            <th scope="col">Attempts</th>
            <th scope="col">Created</th>
            <th scope="col">Timing</th>
            <th scope="col">Key</th>
            <th scope="col"><span class="sr-only">Actions</span></th>
          </tr>
        </thead>
        <tbody>
          <TableState v-if="!page || page.jobs.length === 0" :colspan="7" :rows="page?.jobs ?? null" :loading="poll.loading.value" empty="No jobs match." />
          <tr v-for="j in page?.jobs ?? []" :key="j.id" class="jobs-list-row">
            <th scope="row" class="font-normal">
              <JobLink :id="j.id" :label="jobKindLabel(j.kind)" />
              <span class="block font-mono text-xs text-ink-muted">{{ j.kind }}, job {{ j.id }}</span>
            </th>
            <td class="whitespace-nowrap">
              <span :class="jobStateBadgeClass(j)">{{ jobStateLabel(j) }}</span>
              <span v-if="j.state === 'failed' && j.last_error" class="block max-w-xs whitespace-normal break-words text-xs text-danger">{{ j.last_error }}</span>
            </td>
            <td class="whitespace-nowrap">{{ attemptsLabel(j) }}</td>
            <td class="whitespace-nowrap text-xs text-ink-muted">
              <RelativeTime v-if="j.created_at" :value="j.created_at" :now="now" />
            </td>
            <td class="jobs-timing whitespace-nowrap text-xs text-ink-muted">
              <template v-if="j.finished_at">ended <RelativeTime :value="j.finished_at" :now="now" /></template>
              <template v-else-if="j.state === 'running'">
                running{{ j.running_seconds === null ? '' : ` for ${secondsLabel(j.running_seconds)}` }}
                <span v-if="j.worker_id" class="block font-mono">{{ j.worker_id }}</span>
                <span v-if="leaseExpired(j)" class="block font-semibold text-danger">lease {{ leaseLabel(j) }}</span>
              </template>
              <template v-else-if="j.state === 'queued' && j.run_at">due <RelativeTime :value="j.run_at" :now="now" /></template>
            </td>
            <td class="max-w-xs break-all font-mono text-xs text-ink-muted">{{ j.schedule_key ?? j.dedupe_key ?? '' }}</td>
            <td class="whitespace-nowrap text-right">
              <button v-if="j.retryable" type="button" class="btn-ghost btn-sm" :disabled="busy" @click="retry(j)">
                Retry now<span class="sr-only"> job {{ j.id }}</span>
              </button>
              <button v-if="j.cancellable" type="button" class="btn-ghost btn-sm" :disabled="busy" @click="cancel(j)">
                Cancel<span class="sr-only"> job {{ j.id }}</span>
              </button>
            </td>
          </tr>
        </tbody>
      </table>
    </div>
    <nav v-if="page" aria-label="Job pages" class="flex flex-wrap items-center justify-between gap-3 text-sm text-ink-muted">
      <span class="jobs-page-summary">{{ pageSummary }}</span>
      <span class="flex gap-2">
        <button type="button" class="btn-secondary btn-sm" :disabled="cursors.length === 0" @click="newerPage">Newer</button>
        <button type="button" class="btn-secondary btn-sm" :disabled="page.next_before === null" @click="olderPage">Older</button>
      </span>
    </nav>

    <details class="jobs-prune">
      <summary class="text-sm font-medium text-accent hover:text-ink-bright">Prune finished jobs</summary>
      <form class="mt-3 flex flex-wrap items-end gap-2" @submit.prevent="prune">
        <label>
          <span class="field-label">Keep the last</span>
          <select v-model.number="keepDays" name="keep_days" class="field w-auto">
            <option v-for="d in KEEP_DAYS" :key="d" :value="d">{{ d }} day{{ d === 1 ? '' : 's' }}</option>
          </select>
        </label>
        <button type="submit" class="btn-danger btn-sm" :disabled="busy">Prune now…</button>
      </form>
    </details>
  </section>
</template>

<script setup lang="ts">
// The jobs page's Jobs view: a page of jobs, newest first, filtered by the
// URL's query (?tab=jobs&state=&kind=&from=&to=&q=&failure=), each with its
// retry or cancel; and the prune. Read every 15 s, every 5 s while one moves.
import { computed, reactive, ref, watch } from 'vue';
import { RouterLink, useRoute, useRouter, type LocationQueryRaw } from 'vue-router';
import RelativeTime from '../../../components/RelativeTime.vue';
import JobLink from '../../../components/links/JobLink.vue';
import TableState from '../../../components/ui/TableState.vue';
import type { AdminResult } from '../../../api/admin';
import { cancelJob, fetchJobs, pruneJobs, retryJob } from '../../../api/jobs';
import { confirmAction } from '../../../composables/useConfirm';
import { usePolling } from '../../../composables/usePolling';
import type { Job, JobPage, JobQuery, JobState } from '../../../types/jobs';
import { formatCount } from '../../../utils/format';
import {
  attemptsLabel,
  jobKindLabel,
  jobStateBadgeClass,
  jobStateLabel,
  leaseExpired,
  leaseLabel,
  secondsLabel,
  utcInputSeconds,
  utcInputValue,
} from '../../../utils/jobsView';

const props = defineProps<{
  // The kinds the filter offers.
  kinds: string[];
  now: number;
}>();

const emit = defineEmits<{
  notice: [notice: { ok: boolean; text: string }];
  changed: [];
}>();

const BUSY_POLL_MS = 5_000;
const IDLE_POLL_MS = 15_000;
const PAGE_SIZE = 50;
const KEEP_DAYS = [1, 3, 7, 14] as const;
const STATES: readonly JobState[] = ['queued', 'running', 'done', 'failed'];
const PATH = '/admin/system/jobs';

const route = useRoute();
const router = useRouter();

const page = ref<JobPage | null>(null);
const busy = ref(false);
const filterError = ref('');
const keepDays = ref<number>(14);
// The form's fields; the query the list reads is the route's.
const filter = reactive({ state: '' as '' | JobState, kind: '', from: '', to: '', q: '' });
// The page's cursor, and the `before` of each page before this one (the newest first is none).
const before = ref<number | undefined>(undefined);
const cursors = ref<(number | undefined)[]>([]);

function text(value: unknown): string {
  return typeof value === 'string' ? value : '';
}

function seconds(value: unknown): number | undefined {
  const t = text(value);
  return /^\d{1,12}$/.test(t) ? Number(t) : undefined;
}

// The list's filter, read from the route's query (what it cannot read is left out).
const query = computed<JobQuery>(() => {
  const q = route.query;
  const out: JobQuery = {};
  const state = text(q.state);
  if (STATES.includes(state as JobState)) out.state = state as JobState;
  if (text(q.kind)) out.kind = text(q.kind);
  const from = seconds(q.from);
  if (from !== undefined) out.from = from;
  const to = seconds(q.to);
  if (to !== undefined) out.to = to;
  if (text(q.q).trim()) out.q = text(q.q).trim();
  if (text(q.failure)) out.failure = text(q.failure);
  return out;
});

// The query as the route carries it, on this view.
function routeQuery(q: JobQuery): LocationQueryRaw {
  const out: LocationQueryRaw = { tab: 'jobs' };
  for (const [key, value] of Object.entries(q)) {
    if (value !== undefined && value !== '') out[key] = String(value);
  }
  return out;
}

const withoutFailure = computed(() => {
  const rest: JobQuery = { ...query.value };
  delete rest.failure;
  return { path: PATH, query: routeQuery(rest) };
});

// The kinds offered, the query's own among them (a link may name one the
// overview has not listed yet).
const kindChoices = computed(() => {
  const kind = query.value.kind;
  return kind && !props.kinds.includes(kind) ? [...props.kinds, kind].sort() : props.kinds;
});

const pageSummary = computed(() => {
  const p = page.value;
  if (!p) return '';
  if (p.jobs.length === 0) return `${formatCount(p.total)} jobs`;
  const first = cursors.value.length * PAGE_SIZE + 1;
  const last = first + p.jobs.length - 1;
  return `${formatCount(first)} to ${formatCount(last)} of ${formatCount(p.total)}`;
});

function syncForm() {
  const q = query.value;
  Object.assign(filter, {
    state: q.state ?? '',
    kind: q.kind ?? '',
    from: q.from !== undefined ? utcInputValue(q.from) : '',
    to: q.to !== undefined ? utcInputValue(q.to) : '',
    q: q.q ?? '',
  });
  filterError.value = '';
}
syncForm();

const poll = usePolling({
  async read(signal, background) {
    const result = await fetchJobs({ ...query.value, ...(before.value !== undefined ? { before: before.value } : {}), limit: PAGE_SIZE }, signal);
    if (signal.aborted) return false;
    if (result.ok) {
      page.value = result.value;
      return true;
    }
    if (!background || page.value === null) emit('notice', { ok: false, text: result.error.message });
    return false;
  },
  next: () => (page.value?.jobs.some((j) => j.state === 'queued' || j.state === 'running') ? BUSY_POLL_MS : IDLE_POLL_MS),
});

function firstPage() {
  before.value = undefined;
  cursors.value = [];
  void poll.refresh();
}

// A new filter (Apply, a link, Back): its first page.
watch(
  () => JSON.stringify(query.value),
  () => {
    syncForm();
    firstPage();
  },
);

function applyFilter() {
  const from = utcInputSeconds(filter.from);
  const to = utcInputSeconds(filter.to);
  if ((filter.from && from === null) || (filter.to && to === null)) {
    filterError.value = 'Pick the date again.';
    return;
  }
  if (from !== null && to !== null && to <= from) {
    filterError.value = 'The end comes before the start.';
    return;
  }
  filterError.value = '';
  const q = filter.q.trim();
  const next: JobQuery = {
    ...(filter.state ? { state: filter.state } : {}),
    ...(filter.kind ? { kind: filter.kind } : {}),
    ...(from !== null ? { from } : {}),
    ...(to !== null ? { to } : {}),
    ...(q ? { q } : {}),
    ...(query.value.failure ? { failure: query.value.failure } : {}),
  };
  // The same filter: its first page again.
  if (JSON.stringify(routeQuery(next)) === JSON.stringify(routeQuery(query.value))) firstPage();
  else void router.push({ path: PATH, query: routeQuery(next) });
}

function resetFilter() {
  void router.push({ path: PATH, query: routeQuery({}) });
  // No filter to drop: the watch sees no change, so read again here.
  if (Object.keys(query.value).length === 0) {
    syncForm();
    firstPage();
  }
}

function olderPage() {
  const next = page.value?.next_before;
  if (next === null || next === undefined) return;
  cursors.value = [...cursors.value, before.value];
  before.value = next;
  void poll.refresh();
}

function newerPage() {
  if (cursors.value.length === 0) return;
  before.value = cursors.value[cursors.value.length - 1];
  cursors.value = cursors.value.slice(0, -1);
  void poll.refresh();
}

async function act(run: () => Promise<AdminResult<unknown>>, done: string) {
  busy.value = true;
  const result = await run();
  busy.value = false;
  emit('notice', result.ok ? { ok: true, text: done } : { ok: false, text: result.error.message });
  await poll.readAgain();
  emit('changed');
}

async function retry(j: Job) {
  if (busy.value) return;
  if (!await confirmAction(`Retry "${jobKindLabel(j.kind)}" (job ${j.id}) now? It gets attempt ${j.attempts + 1}.`)) return;
  await act(() => retryJob(j.id), `Job ${j.id} is queued again.`);
}

async function cancel(j: Job) {
  if (busy.value) return;
  if (!await confirmAction(`Cancel "${jobKindLabel(j.kind)}" (job ${j.id})? It will not run.`)) return;
  await act(() => cancelJob(j.id), `Job ${j.id} is cancelled.`);
}

async function prune() {
  if (busy.value) return;
  const days = keepDays.value;
  const plural = days === 1 ? '' : 's';
  if (!await confirmAction(`Delete every job that finished more than ${days} day${plural} ago? Their history is gone for good.`)) return;
  busy.value = true;
  const result = await pruneJobs(days);
  busy.value = false;
  emit('notice', result.ok ? { ok: true, text: `Pruning jobs older than ${days} day${plural} (job ${result.value.id}).` } : { ok: false, text: result.error.message });
  await poll.readAgain();
  emit('changed');
}

defineExpose({
  refresh: () => void poll.refresh(),
});
</script>
