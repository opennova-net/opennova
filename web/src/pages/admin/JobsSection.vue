<template>
  <div class="space-y-8">
    <header class="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
      <div>
        <h1 class="text-3xl font-semibold text-ink">Background jobs</h1>
        <p class="text-sm text-ink-muted">
          Work NovaWorld does in the background: cloud calls, the Stripe sync, mail and prunes.
        </p>
      </div>
      <div class="flex items-center gap-3">
        <p v-if="loadedAt" class="text-xs text-ink-muted">Updated <RelativeTime :value="loadedAt" :now="now" /></p>
        <button type="button" :class="secondaryButton" :disabled="poll.loading.value" @click="refresh">
          {{ poll.loading.value ? 'Refreshing…' : 'Refresh' }}
        </button>
      </div>
    </header>

    <!-- Live regions, always in the page so a screen reader hears what lands in
         them; the boxes below are their visible copies. -->
    <div role="status" class="sr-only">{{ message }}</div>
    <div role="alert" class="sr-only">{{ error }}</div>
    <div v-if="message" aria-hidden="true" class="rounded-panel border border-online/40 bg-online/10 p-4 text-sm text-online">{{ message }}</div>
    <div v-if="error" aria-hidden="true" class="rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger">{{ error }}</div>
    <p v-if="pollTrouble" class="text-xs text-warn">The last refresh did not get through; this page keeps trying.</p>

    <ul v-if="warnings.length" class="jobs-warnings space-y-2">
      <li v-for="w in warnings" :key="w" class="rounded-panel border border-warn/40 bg-warn/10 p-4 text-sm text-warn">{{ w }}</li>
    </ul>

    <dl v-if="overview" class="grid grid-cols-1 gap-3 min-[420px]:grid-cols-2 lg:grid-cols-4">
      <StatTile label="Queued" :value="formatCount(overview.totals.queued)" :detail="queuedDetail" />
      <StatTile label="Running" :value="formatCount(overview.totals.running)" :detail="runningDetail" />
      <StatTile label="Failed, needs attention" :value="formatCount(overview.attention_total)" :detail="failedDetail" />
      <StatTile
        label="Oldest queued wait"
        :value="overview.totals.oldest_wait_seconds === null ? 'None waiting' : secondsLabel(overview.totals.oldest_wait_seconds)"
        detail="How long the oldest due job has waited for a worker"
      />
    </dl>

    <nav aria-label="Background jobs views" class="border-b border-border">
      <ul class="-mb-px flex gap-1 overflow-x-auto">
        <li v-for="t in TABS" :key="t.id">
          <RouterLink
            :to="tabLink(t.id)"
            class="jobs-tab block whitespace-nowrap border-b-2 px-3 py-2 text-sm font-medium transition"
            :class="tab === t.id ? 'border-accent text-ink' : 'border-transparent text-ink-muted hover:text-ink'"
            :aria-current="tab === t.id ? 'page' : undefined"
          >
            {{ t.label }}
          </RouterLink>
        </li>
      </ul>
    </nav>

    <JobsOverviewTab
      v-if="tab === 'overview'"
      :overview="overview"
      :loading="poll.loading.value"
      :now="now"
      @notice="onNotice"
      @changed="poll.readAgain"
    />
    <JobsListTab v-else-if="tab === 'jobs'" ref="listTab" :kinds="kindOptions" :now="now" @notice="onNotice" @changed="poll.readAgain" />
    <JobsSchedulesTab v-else :overview="overview" :now="now" @notice="onNotice" @changed="poll.readAgain" />
  </div>
</template>

<script setup lang="ts">
// /admin/jobs: the background jobs (GET /api/admin/jobs/overview). Above the
// tabs, at a glance: how many wait, how many run, how many failed and need
// looking at, and how long the oldest due job has waited; the warnings (a
// lease run out, a long wait, a scheduler gone quiet). The tabs, one at a time
// (?tab=, Overview without one): Overview (the failures grouped, the worker
// processes, each kind's counts), Jobs (the filtered, paged list and the
// prune) and Schedules (each periodic kind, run now). The overview is polled
// every 15 s, every 5 s while a job waits or runs; the Jobs tab reads its own
// page.
import { computed, ref } from 'vue';
import { RouterLink, useRoute } from 'vue-router';
import RelativeTime from '../../components/RelativeTime.vue';
import StatTile from '../../components/charts/StatTile.vue';
import JobsListTab from './JobsListTab.vue';
import JobsOverviewTab from './JobsOverviewTab.vue';
import JobsSchedulesTab from './JobsSchedulesTab.vue';
import { fetchJobsOverview } from '../../api/jobs';
import { usePolling } from '../../composables/usePolling';
import type { JobsOverview } from '../../types/jobs';
import { formatCount, formatDuration, timeMs } from '../../utils/format';
import { secondsLabel } from '../../utils/jobsView';

type TabId = 'overview' | 'jobs' | 'schedules';

const TABS: readonly { id: TabId; label: string }[] = [
  { id: 'overview', label: 'Overview' },
  { id: 'jobs', label: 'Jobs' },
  { id: 'schedules', label: 'Schedules' },
];

const BUSY_POLL_MS = 5_000;
const IDLE_POLL_MS = 15_000;
// The oldest due job's wait past which the page warns.
const LATENCY_WARN_S = 60;

const secondaryButton =
  'rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40';

const route = useRoute();

const overview = ref<JobsOverview | null>(null);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const message = ref('');
const error = ref('');
const pollTrouble = ref(false);
const listTab = ref<InstanceType<typeof JobsListTab> | null>(null);
// The error banner shows a failed read (not an action's refusal).
let errorFromLoad = false;

const tab = computed<TabId>(() => (route.query.tab === 'jobs' || route.query.tab === 'schedules' ? route.query.tab : 'overview'));

function tabLink(id: TabId) {
  return { path: '/admin/jobs', query: id === 'overview' ? {} : { tab: id } };
}

const queuedDetail = computed(() => {
  const t = overview.value?.totals;
  if (!t || t.queued === 0) return 'None waiting';
  const later = t.queued - t.due;
  return later > 0 ? `${formatCount(t.due)} due now, ${formatCount(later)} to run later` : `${formatCount(t.due)} due now`;
});

const runningDetail = computed(() => {
  const workers = overview.value?.workers ?? [];
  if (workers.length === 0) return 'No worker seen in the last hour';
  const threads = workers.reduce((n, w) => n + w.threads, 0);
  const busy = workers.reduce((n, w) => n + w.busy, 0);
  return `${busy} of ${threads} worker thread${threads === 1 ? '' : 's'} busy`;
});

const failedDetail = computed(() => {
  const o = overview.value;
  if (!o || o.attention_total === 0) return 'No job failed in the last day';
  const groups = o.attention.length;
  return `In the last day, ${groups} kind${groups === 1 ? '' : 's'} of failure`;
});

// The kinds the list's filter offers: every kind the queue holds and every schedule's.
const kindOptions = computed(() => {
  const kinds = new Set<string>();
  for (const k of overview.value?.kinds ?? []) kinds.add(k.kind);
  for (const s of overview.value?.schedules ?? []) kinds.add(s.kind);
  return [...kinds].sort();
});

// What an operator should look at first.
const warnings = computed(() => {
  const o = overview.value;
  if (!o) return [];
  const out: string[] = [];
  const lapsed = o.workers.reduce((n, w) => n + w.lapsed, 0);
  if (lapsed > 0) {
    out.push(
      `${lapsed} running job${lapsed === 1 ? "'s" : "s'"} lease ran out: ${lapsed === 1 ? 'its worker' : 'their workers'} went away. The keeper queues ${lapsed === 1 ? 'it' : 'them'} again within seconds; a job stuck here means no worker runs.`,
    );
  }
  const wait = o.totals.oldest_wait_seconds;
  if (wait !== null && wait >= LATENCY_WARN_S) {
    out.push(`The oldest due job has waited ${secondsLabel(wait)} for a worker: the workers are busy, or none runs.`);
  }
  // The worker and the data program schedule every periodic kind; none enqueued for three of the
  // shortest intervals (two minutes at least) is one not leading.
  if (o.schedules.length > 0) {
    const shortest = Math.min(...o.schedules.map((s) => s.interval_seconds));
    const allowed = Math.max(3 * shortest, 120) * 1000;
    const last = o.last_scheduled_at ? timeMs(o.last_scheduled_at) : Number.NaN;
    const at = timeMs(o.at);
    if (Number.isNaN(last)) {
      out.push('No schedule has enqueued a job yet. Is a worker running?');
    } else if (at - last > allowed) {
      out.push(`No schedule has enqueued a job for ${formatDuration(at - last)}. Is a worker running?`);
    }
  }
  return out;
});

function onNotice(notice: { ok: boolean; text: string }) {
  if (notice.ok) {
    message.value = notice.text;
    error.value = '';
  } else {
    error.value = notice.text;
    message.value = '';
  }
  errorFromLoad = false;
}

const poll = usePolling({
  async read(signal, background) {
    const result = await fetchJobsOverview(signal);
    if (signal.aborted) return false;
    now.value = Date.now();
    if (result.ok) {
      overview.value = result.value;
      loadedAt.value = now.value;
      pollTrouble.value = false;
      if (errorFromLoad) {
        error.value = '';
        errorFromLoad = false;
      }
      return true;
    }
    if (background && overview.value !== null) {
      pollTrouble.value = true;
    } else {
      onNotice({ ok: false, text: result.error.message });
      errorFromLoad = true;
    }
    return false;
  },
  next: () => {
    const t = overview.value?.totals;
    return t && (t.running > 0 || t.due > 0) ? BUSY_POLL_MS : IDLE_POLL_MS;
  },
});

function refresh() {
  void poll.refresh();
  listTab.value?.refresh();
}
</script>
