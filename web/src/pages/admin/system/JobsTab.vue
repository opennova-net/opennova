<template>
  <div class="space-y-6">
    <PageHeader title="System" :tabs="SYSTEM_TABS" refreshable :loaded-at="loadedAt" :now="now" :loading="poll.loading.value" @refresh="refresh" />

    <LiveNotices :message="message" :error="error" :poll-trouble="pollTrouble" />

    <ul v-if="warnings.length" class="jobs-warnings space-y-2">
      <li v-for="w in warnings" :key="w" class="notice notice-warn">{{ w }}</li>
    </ul>

    <dl v-if="overview" class="jobs-tiles grid grid-cols-1 gap-3 min-[420px]:grid-cols-2 lg:grid-cols-4">
      <StatTile label="Queued" :to="viewLink('jobs', { state: 'queued' })" :value="formatCount(overview.totals.queued)" :detail="queuedDetail" />
      <StatTile label="Running" :to="viewLink('jobs', { state: 'running' })" :value="formatCount(overview.totals.running)" :detail="runningDetail" />
      <StatTile
        label="Failed today"
        :to="viewLink('jobs', { state: 'failed' })"
        :value="formatCount(overview.attention_total)"
        :detail="failedDetail"
        :tone="overview.attention_total > 0 ? 'warn' : undefined"
      />
      <StatTile
        label="Oldest wait"
        :value="overview.totals.oldest_wait_seconds === null ? 'None' : secondsLabel(overview.totals.oldest_wait_seconds)"
        :tone="(overview.totals.oldest_wait_seconds ?? 0) >= LATENCY_WARN_S ? 'warn' : undefined"
      />
    </dl>

    <nav aria-label="Jobs views" class="jobs-views">
      <ul class="inline-flex flex-wrap gap-1 rounded-panel border border-border bg-surface p-1">
        <li v-for="v in VIEWS" :key="v.id">
          <RouterLink
            :to="viewLink(v.id)"
            class="block rounded-control px-3 py-1.5 text-sm transition"
            :class="tab === v.id ? 'bg-raised font-semibold text-ink' : 'text-ink-muted hover:text-ink'"
            :aria-current="tab === v.id ? 'page' : undefined"
          >
            {{ v.label }}
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
// /admin/system/jobs: the background jobs at a glance (the queue's tiles and
// what to look at first), then one view at a time (?tab=): Overview, Jobs or
// Schedules. Polled every 15 s, every 5 s while a job waits or runs.
import { computed, ref } from 'vue';
import { RouterLink, useRoute, type LocationQueryRaw } from 'vue-router';
import StatTile from '../../../components/charts/StatTile.vue';
import LiveNotices from '../../../components/ui/LiveNotices.vue';
import PageHeader from '../../../components/ui/PageHeader.vue';
import JobsListTab from './JobsListTab.vue';
import JobsOverviewTab from './JobsOverviewTab.vue';
import JobsSchedulesTab from './JobsSchedulesTab.vue';
import { fetchJobsOverview } from '../../../api/jobs';
import { usePolling } from '../../../composables/usePolling';
import type { JobsOverview } from '../../../types/jobs';
import { SYSTEM_TABS } from '../../../utils/adminNav';
import { formatCount, formatDuration, timeMs } from '../../../utils/format';
import { secondsLabel } from '../../../utils/jobsView';

type ViewId = 'overview' | 'jobs' | 'schedules';

const VIEWS: readonly { id: ViewId; label: string }[] = [
  { id: 'overview', label: 'Overview' },
  { id: 'jobs', label: 'Jobs' },
  { id: 'schedules', label: 'Schedules' },
];

const BUSY_POLL_MS = 5_000;
const IDLE_POLL_MS = 15_000;
// The oldest due job's wait past which the page warns.
const LATENCY_WARN_S = 60;

const route = useRoute();

const overview = ref<JobsOverview | null>(null);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const message = ref('');
const error = ref('');
const pollTrouble = ref(false);
const listTab = ref<InstanceType<typeof JobsListTab> | null>(null);
// The error box shows a failed read (not an action's refusal).
let errorFromLoad = false;

const tab = computed<ViewId>(() => (route.query.tab === 'jobs' || route.query.tab === 'schedules' ? route.query.tab : 'overview'));

function viewLink(id: ViewId, filter: LocationQueryRaw = {}) {
  return { path: '/admin/system/jobs', query: id === 'overview' ? {} : { tab: id, ...filter } };
}

const queuedDetail = computed(() => {
  const t = overview.value?.totals;
  if (!t || t.queued === 0) return undefined;
  const later = t.queued - t.due;
  return later > 0 ? `${formatCount(t.due)} due, ${formatCount(later)} later` : `${formatCount(t.due)} due`;
});

const runningDetail = computed(() => {
  const workers = overview.value?.workers ?? [];
  if (workers.length === 0) return 'No worker seen';
  const threads = workers.reduce((n, w) => n + w.threads, 0);
  const busy = workers.reduce((n, w) => n + w.busy, 0);
  return `${busy} of ${threads} threads busy`;
});

const failedDetail = computed(() => {
  const groups = overview.value?.attention.length ?? 0;
  return groups > 0 ? `${groups} kind${groups === 1 ? '' : 's'} of failure` : undefined;
});

// The kinds the list's filter offers: every kind the queue holds and every schedule's.
const kindOptions = computed(() => {
  const kinds = new Set<string>();
  for (const k of overview.value?.kinds ?? []) kinds.add(k.kind);
  for (const s of overview.value?.schedules ?? []) kinds.add(s.kind);
  return [...kinds].sort();
});

// What to look at first, a sentence each.
const warnings = computed(() => {
  const o = overview.value;
  if (!o) return [];
  const out: string[] = [];
  const lapsed = o.workers.reduce((n, w) => n + w.lapsed, 0);
  if (lapsed > 0) out.push(lapsed === 1 ? "1 running job's lease ran out." : `${lapsed} running jobs' leases ran out.`);
  const wait = o.totals.oldest_wait_seconds;
  if (wait !== null && wait >= LATENCY_WARN_S) out.push(`The oldest due job has waited ${secondsLabel(wait)}.`);
  // No schedule's job for three of the shortest intervals (two minutes at
  // least): no worker leads.
  if (o.schedules.length > 0) {
    const shortest = Math.min(...o.schedules.map((s) => s.interval_seconds));
    const allowed = Math.max(3 * shortest, 120) * 1000;
    const last = o.last_scheduled_at ? timeMs(o.last_scheduled_at) : Number.NaN;
    const at = timeMs(o.at);
    if (Number.isNaN(last)) out.push('No schedule has enqueued a job yet.');
    else if (at - last > allowed) out.push(`No schedule has enqueued a job for ${formatDuration(at - last)}.`);
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
