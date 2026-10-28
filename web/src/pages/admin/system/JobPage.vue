<template>
  <div class="space-y-6">
    <PageHeader :title="job ? jobKindLabel(job.kind) : `Job ${id}`" :crumbs="crumbs" refreshable :loaded-at="loadedAt" :now="now" :loading="poll.loading.value" @refresh="poll.refresh">
      <template v-if="job" #meta>
        <span>Job {{ job.id }}</span>
        <span class="font-mono">{{ job.kind }}</span>
        <span :class="jobStateBadgeClass(job)">{{ jobStateLabel(job) }}</span>
        <span v-if="job.manual">Run by an admin</span>
      </template>
      <template v-if="job && (job.retryable || job.cancellable)" #actions>
        <button v-if="job.retryable" type="button" class="btn-primary" :disabled="busy" @click="retry">Retry now…</button>
        <button v-if="job.cancellable" type="button" class="btn-danger" :disabled="busy" @click="cancel">Cancel…</button>
      </template>
    </PageHeader>

    <LiveNotices :message="message" :error="error" />

    <div v-if="job === null && unread" class="job-missing card flex flex-wrap items-center justify-between gap-3 p-4 text-sm">
      <p>The job could not be loaded.</p>
      <button type="button" class="btn-secondary btn-sm" @click="poll.refresh">Try again</button>
    </div>

    <template v-if="job">
      <dl class="job-fields card grid gap-x-6 gap-y-2 p-4 text-sm sm:grid-cols-[10rem_1fr]">
        <dt class="text-ink-muted">Attempts</dt>
        <dd>{{ attemptsLabel(job) }}</dd>
        <template v-for="t in times" :key="t.label">
          <dt class="text-ink-muted">{{ t.label }}</dt>
          <dd>
            <template v-if="t.value">
              <RelativeTime :value="t.value" :now="now" />
              <span class="ml-2 text-xs text-ink-muted">{{ formatUtc(t.value) }}</span>
            </template>
            <span v-else class="text-ink-muted">Not yet</span>
          </dd>
        </template>
        <dt class="text-ink-muted">Worker</dt>
        <dd class="font-mono text-xs">{{ job.worker_id ?? 'None yet' }}</dd>
        <template v-if="job.state === 'running'">
          <dt class="text-ink-muted">Running for</dt>
          <dd>{{ job.running_seconds === null ? 'Unknown' : secondsLabel(job.running_seconds) }}</dd>
          <dt class="text-ink-muted">Lease</dt>
          <dd :class="leaseExpired(job) ? 'font-semibold text-danger' : ''">{{ capitalized(leaseLabel(job)) }}</dd>
        </template>
        <dt class="text-ink-muted">Dedupe key</dt>
        <dd class="break-all font-mono text-xs">{{ job.dedupe_key ?? 'None' }}</dd>
        <dt class="text-ink-muted">Schedule key</dt>
        <dd class="break-all font-mono text-xs">{{ job.schedule_key ?? 'None' }}</dd>
        <dt class="text-ink-muted">Secret</dt>
        <dd>{{ secretLabel }}</dd>
        <dt class="text-ink-muted">Last error</dt>
        <dd :class="job.last_error ? 'whitespace-pre-wrap break-words text-danger' : 'text-ink-muted'">{{ job.last_error || 'None' }}</dd>
      </dl>

      <section class="space-y-3" aria-labelledby="job-payload">
        <h2 id="job-payload" class="section-heading">Payload</h2>
        <ul v-if="links.length" class="job-payload-links flex flex-wrap gap-x-4 gap-y-1 text-sm">
          <li v-for="l in links" :key="`${l.kind} ${l.id}`" class="flex items-baseline gap-1.5">
            <span class="text-xs text-ink-muted">{{ LINK_LABELS[l.kind] }}</span>
            <MachineLink v-if="l.kind === 'machine'" :id="l.id" :label="l.name" />
            <ServerLink v-else-if="l.kind === 'server'" :id="l.id" :name="l.name" scope="admin" />
            <PlayerLink v-else-if="l.kind === 'player'" :id="l.id" :name="l.name" />
            <ProductLink v-else-if="l.kind === 'product'" :product-ref="l.id" :name="l.name" />
            <JobLink v-else :id="l.id" :label="l.name" />
          </li>
        </ul>
        <pre class="job-payload overflow-x-auto rounded-panel border border-border bg-panel p-4 font-mono text-xs text-ink">{{ payloadText }}</pre>
      </section>
    </template>
  </div>
</template>

<script setup lang="ts">
// /admin/jobs/<id>: one background job, its times, worker, keys, error and
// payload (with links to what the payload names), and its retry or cancel.
// Read again every 5 s while it waits or runs.
import { computed, ref } from 'vue';
import { useRoute } from 'vue-router';
import RelativeTime from '../../../components/RelativeTime.vue';
import JobLink from '../../../components/links/JobLink.vue';
import MachineLink from '../../../components/links/MachineLink.vue';
import PlayerLink from '../../../components/links/PlayerLink.vue';
import ProductLink from '../../../components/links/ProductLink.vue';
import ServerLink from '../../../components/links/ServerLink.vue';
import LiveNotices from '../../../components/ui/LiveNotices.vue';
import PageHeader from '../../../components/ui/PageHeader.vue';
import { cancelJob, fetchJob, retryJob } from '../../../api/jobs';
import { confirmAction } from '../../../composables/useConfirm';
import { usePolling } from '../../../composables/usePolling';
import type { Job } from '../../../types/jobs';
import type { Crumb } from '../../../utils/adminNav';
import { formatUtc } from '../../../utils/format';
import {
  attemptsLabel,
  jobKindLabel,
  jobStateBadgeClass,
  jobStateLabel,
  leaseExpired,
  leaseLabel,
  payloadLinks,
  secondsLabel,
  type PayloadLinkKind,
} from '../../../utils/jobsView';

const LIVE_POLL_MS = 5_000;

const LINK_LABELS: Record<PayloadLinkKind, string> = {
  machine: 'Machine',
  server: 'Server',
  player: 'Player',
  product: 'Product',
  job: 'Job',
};

const route = useRoute();
const id = computed(() => Number(route.params.id));

const job = ref<Job | null>(null);
const busy = ref(false);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const message = ref('');
const error = ref('');
// The first read failed: the page offers another.
const unread = ref(false);

// System / Jobs, then the Jobs view of its kind once it is read.
const crumbs = computed<Crumb[]>(() => [
  { label: 'System', to: '/admin/system' },
  { label: 'Jobs', to: '/admin/system/jobs' },
  ...(job.value ? [{ label: `${jobKindLabel(job.value.kind)} jobs`, to: { path: '/admin/system/jobs', query: { tab: 'jobs', kind: job.value.kind } } }] : []),
]);

// Its times, each relative with its UTC time beside it, or not yet.
const times = computed(() => {
  const j = job.value;
  if (!j) return [];
  return [
    { label: 'Runs from', value: j.run_at },
    { label: 'Created', value: j.created_at },
    { label: 'Last taken', value: j.started_at },
    { label: 'Ended', value: j.finished_at },
  ];
});

const secretLabel = computed(() => {
  const j = job.value;
  switch (j?.secret_state) {
    case 'held':
      return 'Held, never shown';
    case 'dropped':
      return j.state === 'failed' ? 'Dropped: it cannot run again' : 'Dropped when it ended';
    default:
      return 'None';
  }
});

const payloadText = computed(() => {
  const payload = job.value?.payload;
  if (payload === undefined) return '{}';
  return typeof payload === 'string' ? payload : JSON.stringify(payload, null, 2);
});

const links = computed(() => payloadLinks(job.value?.payload));

function capitalized(text: string): string {
  return text.charAt(0).toUpperCase() + text.slice(1);
}

const poll = usePolling({
  async read(signal, background) {
    const result = await fetchJob(id.value, signal);
    if (signal.aborted) return false;
    now.value = Date.now();
    if (result.ok) {
      job.value = result.value;
      loadedAt.value = now.value;
      unread.value = false;
      return true;
    }
    if (job.value === null) unread.value = true;
    if (!background || job.value === null) {
      error.value = result.error.message;
      message.value = '';
    }
    return false;
  },
  next: () => (job.value && (job.value.state === 'queued' || job.value.state === 'running') ? LIVE_POLL_MS : null),
});

async function act(question: string, run: () => ReturnType<typeof retryJob>, done: string) {
  if (busy.value || !await confirmAction(question)) return;
  busy.value = true;
  const result = await run();
  busy.value = false;
  if (!result.ok) {
    error.value = result.error.message;
    message.value = '';
  } else {
    job.value = result.value;
    message.value = done;
    error.value = '';
  }
  await poll.readAgain();
}

function retry() {
  const j = job.value;
  if (!j) return;
  void act(`Retry "${jobKindLabel(j.kind)}" (job ${j.id}) now? It gets attempt ${j.attempts + 1}.`, () => retryJob(j.id), `Job ${j.id} is queued again.`);
}

function cancel() {
  const j = job.value;
  if (!j) return;
  void act(`Cancel "${jobKindLabel(j.kind)}" (job ${j.id})? It will not run.`, () => cancelJob(j.id), `Job ${j.id} is cancelled.`);
}
</script>
