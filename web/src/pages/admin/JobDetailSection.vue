<template>
  <div class="space-y-10">
    <RouterLink to="/admin/jobs" class="text-sm text-accent hover:text-ink-bright">Background jobs</RouterLink>

    <div role="status" class="sr-only">{{ message }}</div>
    <div role="alert" class="sr-only">{{ error }}</div>
    <div v-if="message" aria-hidden="true" class="rounded-panel border border-online/40 bg-online/10 p-4 text-sm text-online">{{ message }}</div>
    <div v-if="error" aria-hidden="true" class="rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger">{{ error }}</div>

    <div v-if="job === null && poll.loading.value" class="animate-pulse rounded-panel border border-border bg-panel p-8 text-center text-ink-muted">
      Loading the job…
    </div>
    <div v-else-if="job === null" class="space-y-3 rounded-panel border border-border bg-panel p-6">
      <h1 class="text-xl font-semibold text-ink">The job could not be loaded</h1>
      <button type="button" :class="secondaryButton" @click="poll.refresh">Try again</button>
    </div>

    <template v-else>
      <header class="space-y-2">
        <h1 class="break-words text-3xl font-semibold text-ink">{{ jobKindLabel(job.kind) }}</h1>
        <p class="flex flex-wrap items-center gap-2 text-sm text-ink-muted">
          <span>Job {{ job.id }}</span>
          <span class="font-mono">{{ job.kind }}</span>
          <span :class="jobStateBadgeClass(job)">{{ jobStateLabel(job) }}</span>
          <span v-if="job.manual">Run now by an admin</span>
        </p>
      </header>

      <section v-if="job.retryable || job.cancellable" class="flex flex-wrap items-center gap-3" aria-label="Actions">
        <button v-if="job.retryable" type="button" :class="primaryButton" :disabled="busy" @click="retry">
          Retry now: attempt {{ job.attempts + 1 }} of {{ job.attempts + 1 }}…
        </button>
        <button
          v-if="job.cancellable"
          type="button"
          :disabled="busy"
          class="rounded-control border border-danger px-4 py-2 text-sm font-medium text-danger transition hover:bg-danger/10 disabled:opacity-40"
          @click="cancel"
        >
          Cancel the job…
        </button>
      </section>
      <p v-else-if="job.state === 'failed' && job.secret_state === 'dropped'" class="text-sm text-ink-muted">
        This job carried a secret that was dropped when it ended (a mail's body, a machine's launch token), so it cannot
        run again.
      </p>

      <section class="space-y-4" aria-labelledby="job-detail">
        <h2 id="job-detail" class="text-xl font-semibold text-ink">Details</h2>
        <dl class="job-fields grid gap-x-6 gap-y-3 rounded-panel border border-border bg-panel p-4 text-sm sm:grid-cols-[12rem_1fr] sm:p-6">
          <dt :class="termClass">Attempts</dt>
          <dd>{{ attemptsLabel(job) }}</dd>
          <template v-for="t in times" :key="t.label">
            <dt :class="termClass">{{ t.label }}</dt>
            <dd>
              <template v-if="t.value">
                <RelativeTime :value="t.value" :now="now" />
                <span class="ml-2 text-xs text-ink-muted">{{ formatUtc(t.value) }}</span>
              </template>
              <span v-else class="text-ink-muted">Not yet</span>
            </dd>
          </template>
          <dt :class="termClass">Worker</dt>
          <dd class="font-mono text-xs">{{ job.worker_id ?? 'None yet' }}</dd>
          <template v-if="job.state === 'running'">
            <dt :class="termClass">Running for</dt>
            <dd>{{ job.running_seconds === null ? 'Unknown' : secondsLabel(job.running_seconds) }}</dd>
            <dt :class="termClass">Lease</dt>
            <dd :class="leaseExpired(job) ? 'font-semibold text-danger' : ''">
              {{ capitalized(leaseLabel(job)) }}<template v-if="leaseExpired(job)">: its worker went away, and the keeper queues it again</template>
            </dd>
          </template>
          <dt :class="termClass">Dedupe key</dt>
          <dd class="break-all font-mono text-xs">{{ job.dedupe_key ?? 'None' }}</dd>
          <dt :class="termClass">Schedule key</dt>
          <dd class="break-all font-mono text-xs">{{ job.schedule_key ?? 'None' }}</dd>
          <dt :class="termClass">Secret</dt>
          <dd>{{ secretLabel }}</dd>
          <dt :class="termClass">Last error</dt>
          <dd :class="job.last_error ? 'whitespace-pre-wrap break-words text-danger' : 'text-ink-muted'">{{ job.last_error || 'None' }}</dd>
        </dl>
      </section>

      <section class="space-y-4" aria-labelledby="job-payload">
        <h2 id="job-payload" class="text-xl font-semibold text-ink">Payload</h2>
        <p class="text-sm text-ink-muted">What its handler reads. A secret is never part of it.</p>
        <pre class="job-payload overflow-x-auto rounded-panel border border-border bg-panel p-4 font-mono text-xs text-ink">{{ payloadText }}</pre>
      </section>
    </template>
  </div>
</template>

<script setup lang="ts">
// /admin/jobs/<id>: one background job (GET /api/admin/jobs/<id>): its kind,
// state, attempts, its times (when it may run, was made, last taken, ended),
// its worker and lease while it runs, its keys, whether it carries a secret
// (never the secret), its last error and its payload; a retry for a failed
// one (POST .../retry: one more attempt) and a cancel for a waiting one (POST
// .../cancel), each asked first. Read again every 5 s while it waits or runs.
import { computed, ref } from 'vue';
import { RouterLink, useRoute } from 'vue-router';
import RelativeTime from '../../components/RelativeTime.vue';
import { cancelJob, fetchJob, retryJob } from '../../api/jobs';
import { usePolling } from '../../composables/usePolling';
import type { Job } from '../../types/jobs';
import { formatUtc } from '../../utils/format';
import {
  attemptsLabel,
  jobKindLabel,
  jobStateBadgeClass,
  jobStateLabel,
  leaseExpired,
  leaseLabel,
  secondsLabel,
} from '../../utils/jobsView';

const LIVE_POLL_MS = 5_000;

const termClass = 'text-xs font-medium uppercase tracking-wider text-ink-muted';
const primaryButton =
  'rounded-control bg-accent px-4 py-2 text-sm font-medium text-on-accent transition hover:bg-accent/90 disabled:cursor-not-allowed disabled:opacity-50';
const secondaryButton =
  'rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40';

const route = useRoute();
const id = computed(() => Number(route.params.id));

const job = ref<Job | null>(null);
const busy = ref(false);
const now = ref(Date.now());
const message = ref('');
const error = ref('');

// Its times, each relative with its UTC time beside it, or not yet.
const times = computed(() => {
  const j = job.value;
  if (!j) return [];
  return [
    { label: 'Runs from', value: j.run_at },
    { label: 'Created', value: j.created_at },
    { label: 'Last taken by a worker', value: j.started_at },
    { label: 'Ended', value: j.finished_at },
  ];
});

const secretLabel = computed(() => {
  switch (job.value?.secret_state) {
    case 'held':
      return 'It carries one, never shown; it is dropped when the job ends.';
    case 'dropped':
      return 'It carried one, dropped when it ended.';
    default:
      return 'None';
  }
});

const payloadText = computed(() => {
  const payload = job.value?.payload;
  if (payload === undefined) return '{}';
  return typeof payload === 'string' ? payload : JSON.stringify(payload, null, 2);
});

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
      return true;
    }
    if (!background || job.value === null) {
      error.value = result.error.message;
      message.value = '';
    }
    return false;
  },
  next: () => (job.value && (job.value.state === 'queued' || job.value.state === 'running') ? LIVE_POLL_MS : null),
});

async function retry() {
  const j = job.value;
  if (!j || busy.value) return;
  const next = j.attempts + 1;
  if (!window.confirm(`Retry "${jobKindLabel(j.kind)}" (job ${j.id}) now? It gets one more attempt, ${next} of ${next}.`)) return;
  busy.value = true;
  const result = await retryJob(j.id);
  busy.value = false;
  if (!result.ok) {
    error.value = result.error.message;
    message.value = '';
  } else {
    job.value = result.value;
    message.value = `Job ${j.id} is queued again to run now.`;
    error.value = '';
  }
  await poll.readAgain();
}

async function cancel() {
  const j = job.value;
  if (!j || busy.value) return;
  if (!window.confirm(`Cancel "${jobKindLabel(j.kind)}" (job ${j.id})? It will not run, and it is marked failed as cancelled by you.`)) return;
  busy.value = true;
  const result = await cancelJob(j.id);
  busy.value = false;
  if (!result.ok) {
    error.value = result.error.message;
    message.value = '';
  } else {
    job.value = result.value;
    message.value = `Job ${j.id} is cancelled.`;
    error.value = '';
  }
  await poll.readAgain();
}
</script>
