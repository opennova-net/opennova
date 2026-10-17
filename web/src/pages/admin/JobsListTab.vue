<template>
  <div class="space-y-10">
    <!-- Every job, filtered and paged. -->
    <section class="space-y-4" aria-labelledby="jobs-list">
      <h2 id="jobs-list" class="text-xl font-semibold text-ink">Jobs</h2>
      <form class="jobs-filter flex flex-wrap items-end gap-3" @submit.prevent="applyFilter">
        <label class="space-y-1">
          <span :class="labelClass">State</span>
          <select v-model="filter.state" name="state" :class="fieldClass">
            <option value="">Any</option>
            <option value="queued">Waiting</option>
            <option value="running">Running</option>
            <option value="done">Done</option>
            <option value="failed">Failed or cancelled</option>
          </select>
        </label>
        <label class="space-y-1">
          <span :class="labelClass">Kind</span>
          <select v-model="filter.kind" name="kind" :class="fieldClass">
            <option value="">Any</option>
            <option v-for="k in kindChoices" :key="k" :value="k">{{ jobKindLabel(k) }}</option>
          </select>
        </label>
        <label class="space-y-1">
          <span :class="labelClass">Created from (UTC)</span>
          <input v-model="filter.from" name="from" type="datetime-local" :class="fieldClass" />
        </label>
        <label class="space-y-1">
          <span :class="labelClass">Created before (UTC)</span>
          <input v-model="filter.to" name="to" type="datetime-local" :class="fieldClass" />
        </label>
        <label class="space-y-1">
          <span :class="labelClass">Key contains</span>
          <input
            v-model="filter.q"
            name="q"
            type="search"
            autocomplete="off"
            maxlength="128"
            placeholder="A dedupe or schedule key"
            :class="fieldClass"
          />
        </label>
        <button type="submit" :class="secondaryButton">Apply</button>
        <button type="button" :class="secondaryButton" @click="resetFilter">Reset</button>
      </form>
      <p v-if="query.failure" class="jobs-failure-filter flex flex-wrap items-center gap-x-2 gap-y-1 text-sm">
        <span class="text-ink-muted">Only the jobs that failed with</span>
        <span class="break-all font-mono text-xs text-danger">{{ query.failure }}</span>
        <RouterLink :to="withoutFailure" class="text-accent hover:text-ink-bright">Show every job</RouterLink>
      </p>
      <p v-if="filterError" class="text-sm text-danger">{{ filterError }}</p>
      <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
        <table class="min-w-full divide-y divide-border">
          <caption class="sr-only">Jobs, newest first</caption>
          <thead>
            <tr :class="headRow">
              <th scope="col" class="px-4 py-3">Job</th>
              <th scope="col" class="px-4 py-3">State</th>
              <th scope="col" class="px-4 py-3">Attempts</th>
              <th scope="col" class="px-4 py-3">Created</th>
              <th scope="col" class="px-4 py-3">Timing</th>
              <th scope="col" class="px-4 py-3">Key</th>
              <th scope="col" class="px-4 py-3 text-right">Actions</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border text-sm text-ink">
            <tr v-if="page === null">
              <td colspan="7" class="px-4 py-6 text-center text-ink-muted">{{ poll.loading.value ? 'Loading…' : 'Not loaded.' }}</td>
            </tr>
            <tr v-else-if="page.jobs.length === 0">
              <td colspan="7" class="px-4 py-6 text-center text-ink-muted">No job matches.</td>
            </tr>
            <tr v-for="j in page?.jobs ?? []" :key="j.id" class="jobs-list-row align-top hover:bg-hover">
              <th scope="row" class="px-4 py-3 text-left font-normal">
                <RouterLink :to="`/admin/jobs/${j.id}`" class="text-accent hover:text-ink-bright">{{ jobKindLabel(j.kind) }}</RouterLink>
                <span class="block font-mono text-xs text-ink-muted">{{ j.kind }}, job {{ j.id }}</span>
              </th>
              <td class="whitespace-nowrap px-4 py-3">
                <span :class="jobStateBadgeClass(j)">{{ jobStateLabel(j) }}</span>
                <span v-if="j.state === 'failed' && j.last_error" class="block max-w-xs whitespace-normal break-words text-xs text-danger">{{ j.last_error }}</span>
              </td>
              <td class="whitespace-nowrap px-4 py-3">{{ attemptsLabel(j) }}</td>
              <td class="whitespace-nowrap px-4 py-3 text-xs text-ink-muted">
                <RelativeTime v-if="j.created_at" :value="j.created_at" :now="now" />
              </td>
              <td class="jobs-timing whitespace-nowrap px-4 py-3 text-xs text-ink-muted">
                <template v-if="j.finished_at">ended <RelativeTime :value="j.finished_at" :now="now" /></template>
                <template v-else-if="j.state === 'running'">
                  running{{ j.running_seconds === null ? '' : ` for ${secondsLabel(j.running_seconds)}` }}
                  <span v-if="j.worker_id" class="block font-mono">{{ j.worker_id }}</span>
                  <span v-if="leaseExpired(j)" class="block font-semibold text-danger">lease {{ leaseLabel(j) }}</span>
                </template>
                <template v-else-if="j.state === 'queued' && j.run_at">due <RelativeTime :value="j.run_at" :now="now" /></template>
              </td>
              <td class="max-w-xs break-all px-4 py-3 font-mono text-xs text-ink-muted">{{ j.schedule_key ?? j.dedupe_key ?? '' }}</td>
              <td class="space-x-3 whitespace-nowrap px-4 py-3 text-right">
                <button v-if="j.retryable" type="button" :disabled="busy" class="text-accent hover:text-ink-bright disabled:opacity-40" @click="retry(j)">
                  Retry now<span class="sr-only"> job {{ j.id }}</span>
                </button>
                <button v-if="j.cancellable" type="button" :disabled="busy" class="text-danger/80 hover:text-danger disabled:opacity-40" @click="cancel(j)">
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
          <button type="button" :class="secondaryButton" :disabled="cursors.length === 0" @click="newerPage">Newer</button>
          <button type="button" :class="secondaryButton" :disabled="page.next_before === null" @click="olderPage">Older</button>
        </span>
      </nav>
    </section>

    <!-- The prune. -->
    <section class="space-y-4 rounded-panel border border-border p-4 sm:p-6" aria-labelledby="jobs-prune">
      <h2 id="jobs-prune" class="text-xl font-semibold text-ink">Prune finished jobs</h2>
      <p class="text-sm text-ink-muted">
        Finished jobs are pruned after 14 days. Prune sooner here; waiting and running jobs are kept.
      </p>
      <form class="flex flex-wrap items-end gap-3" @submit.prevent="prune">
        <label class="space-y-1">
          <span :class="labelClass">Keep the last</span>
          <select v-model.number="keepDays" name="keep_days" :class="fieldClass">
            <option v-for="d in KEEP_DAYS" :key="d" :value="d">{{ d }} day{{ d === 1 ? '' : 's' }}</option>
          </select>
        </label>
        <button type="submit" :disabled="busy" class="rounded-control border border-danger px-4 py-2 text-sm font-medium text-danger transition hover:bg-danger/10 disabled:opacity-40">
          Prune now…
        </button>
      </form>
    </section>
  </div>
</template>

<script setup lang="ts">
// The background jobs page's Jobs tab (JobsSection.vue): a page of jobs (GET
// /api/admin/jobs) by state, kind, creation time, key and failure, newest
// first, each to its own page (/admin/jobs/<id>), with a retry or a cancel;
// and the prune (POST /api/admin/jobs/prune). The filter is the page's query
// (?tab=jobs&state=&kind=&from=&to=&q=&failure=), so an Overview failure
// group links straight to its jobs; Apply writes it there. Every action asks
// first, then reads again. Read every 15 s, every 5 s while a job it lists
// waits or runs.
import { computed, reactive, ref, watch } from 'vue';
import { RouterLink, useRoute, useRouter, type LocationQueryRaw } from 'vue-router';
import RelativeTime from '../../components/RelativeTime.vue';
import type { AdminResult } from '../../api/admin';
import { cancelJob, fetchJobs, pruneJobs, retryJob } from '../../api/jobs';
import { usePolling } from '../../composables/usePolling';
import type { Job, JobPage, JobQuery, JobState } from '../../types/jobs';
import { formatCount } from '../../utils/format';
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
} from '../../utils/jobsView';

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

const headRow = 'bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted';
const labelClass = 'block text-xs font-medium text-ink-muted';
const fieldClass = 'rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none';
const secondaryButton =
  'rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40';

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

// The query as the route carries it, on this tab.
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
  return { path: '/admin/jobs', query: routeQuery(rest) };
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
  return `Jobs ${formatCount(first)} to ${formatCount(last)} of ${formatCount(p.total)}, newest first`;
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

// A new filter (Apply, a link, Back): its first page.
watch(
  () => JSON.stringify(query.value),
  () => {
    syncForm();
    before.value = undefined;
    cursors.value = [];
    void poll.refresh();
  },
);

function applyFilter() {
  const from = utcInputSeconds(filter.from);
  const to = utcInputSeconds(filter.to);
  if ((filter.from && from === null) || (filter.to && to === null)) {
    filterError.value = 'A date the filter could not read: pick it again.';
    return;
  }
  if (from !== null && to !== null && to <= from) {
    filterError.value = 'The end of the range comes before its start.';
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
  if (JSON.stringify(routeQuery(next)) === JSON.stringify(routeQuery(query.value))) {
    // The same filter: its first page again.
    before.value = undefined;
    cursors.value = [];
    void poll.refresh();
    return;
  }
  void router.push({ path: '/admin/jobs', query: routeQuery(next) });
}

function resetFilter() {
  void router.push({ path: '/admin/jobs', query: routeQuery({}) });
  if (Object.keys(query.value).length === 0) {
    syncForm();
    before.value = undefined;
    cursors.value = [];
    void poll.refresh();
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
  const next = j.attempts + 1;
  if (!window.confirm(`Retry "${jobKindLabel(j.kind)}" (job ${j.id}) now? It gets one more attempt, ${next} of ${next}.`)) return;
  await act(() => retryJob(j.id), `Job ${j.id} is queued again to run now.`);
}

async function cancel(j: Job) {
  if (busy.value) return;
  if (!window.confirm(`Cancel "${jobKindLabel(j.kind)}" (job ${j.id})? It will not run, and it is marked failed as cancelled by you.`)) return;
  await act(() => cancelJob(j.id), `Job ${j.id} is cancelled.`);
}

async function prune() {
  if (busy.value) return;
  const days = keepDays.value;
  const plural = days === 1 ? '' : 's';
  if (!window.confirm(`Delete every job that finished more than ${days} day${plural} ago? Their history goes for good.`)) return;
  busy.value = true;
  const result = await pruneJobs(days);
  busy.value = false;
  emit(
    'notice',
    result.ok
      ? { ok: true, text: `The finished jobs older than ${days} day${plural} are being pruned (job ${result.value.id}).` }
      : { ok: false, text: result.error.message },
  );
  await poll.readAgain();
  emit('changed');
}

defineExpose({
  refresh: () => void poll.refresh(),
});
</script>
