<template>
  <section class="px-4 pt-24 pb-16 sm:px-6">
    <div class="mx-auto max-w-4xl space-y-8">
      <header class="flex flex-col gap-4 sm:flex-row sm:items-end sm:justify-between">
        <div>
          <p class="text-sm uppercase tracking-[0.3em] text-accent">NovaWorld</p>
          <h1 ref="pageHeading" tabindex="-1" class="text-3xl font-semibold text-ink focus:outline-none">Hosted server</h1>
          <p class="mt-1 text-sm text-ink-muted">
            An approved account can have NovaWorld run one Joint Operations server for it, on a machine of its
            own, listed in the server browser until you stop it. Hosted servers are experimental (pre-1.0).
          </p>
        </div>
        <div class="flex items-center gap-3">
          <p v-if="loadedAt" class="text-xs text-ink-muted">
            Updated <time :datetime="new Date(loadedAt).toISOString()">{{ new Date(loadedAt).toLocaleTimeString() }}</time>
          </p>
          <button
            type="button"
            class="rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40"
            :disabled="loading"
            @click="load(false)"
          >
            {{ loading ? 'Refreshing…' : 'Refresh' }}
          </button>
        </div>
      </header>

      <!-- Live regions, always in the page so a screen reader hears what lands in them. -->
      <p role="status" class="sr-only">{{ pageStatus }}</p>
      <p role="alert" class="sr-only">{{ pageError }}</p>
      <p v-if="pageStatus" aria-hidden="true" class="rounded-panel border border-online/40 bg-online/10 p-4 text-sm text-online">
        {{ pageStatus }}
      </p>
      <p v-if="pageError" aria-hidden="true" class="rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger">
        {{ pageError }}
      </p>
      <p v-if="pollTrouble" class="text-xs text-warn">
        The last refresh did not get through; this page keeps trying.
      </p>

      <div
        v-if="view === 'loading'"
        class="animate-pulse rounded-panel border border-border bg-panel p-8 text-center text-ink-muted"
      >
        Loading your hosted server…
      </div>

      <div v-else-if="view === 'error'" class="space-y-3 rounded-panel border border-border bg-panel p-6">
        <h2 class="text-lg font-semibold text-ink">Your hosted server could not be loaded</h2>
        <p class="text-sm text-ink-muted">The message above says why. Refresh to try again.</p>
      </div>

      <template v-else-if="data">
        <!-- A live server: its state, listing, timeline and stop. -->
        <template v-if="view === 'live' && data.server">
          <p v-if="!data.enabled" class="rounded-panel border border-warn/40 bg-warn/10 p-4 text-sm text-warn">
            This NovaWorld service has stopped running hosted servers, so yours may not be looked after until
            it is back.
          </p>
          <p v-if="!data.can_host" class="rounded-panel border border-warn/40 bg-warn/10 p-4 text-sm text-warn">
            Your account's approval to host was withdrawn, so the service is stopping this server.
          </p>
          <HostedServerStatus
            ref="statusCard"
            :server="data.server"
            :now="now"
            @stopped="onStopped"
            @refused="onRefused"
          />
        </template>

        <template v-else>
          <div
            v-if="view === 'off'"
            class="space-y-3 rounded-panel border border-border bg-panel p-6"
          >
            <h2 class="text-lg font-semibold text-ink">Hosting is off</h2>
            <p class="text-sm text-ink-muted">
              This NovaWorld service is not running hosted servers right now, so no server can be started
              here.
            </p>
            <p class="text-sm text-ink-muted">{{ hostYourself }}</p>
          </div>

          <div
            v-else-if="view === 'unapproved'"
            class="space-y-3 rounded-panel border border-border bg-panel p-6"
          >
            <h2 class="text-lg font-semibold text-ink">Hosting is by approval</h2>
            <p class="text-sm text-ink-muted">
              While hosted servers are experimental, an admin approves accounts for them one at a time.
              <span class="font-medium text-ink">{{ user?.username ?? 'Your account' }}</span> is not approved,
              so this page cannot start a server for you yet.
            </p>
            <p class="text-sm text-ink-muted">{{ hostYourself }}</p>
          </div>

          <HostedSettingsForm
            v-else
            :key="formKey"
            :previous="data.server?.settings ?? null"
            :default-missions="data.default_missions"
            @started="onStarted"
            @refused="onRefused"
          />

          <!-- The last one, stopped or failed. -->
          <HostedServerStatus v-if="data.server" :server="data.server" :now="now" />
        </template>
      </template>
    </div>
  </section>
</template>

<script setup lang="ts">
// /me/server: the logged-in account's hosted server (GET /api/me/server), the
// form that starts one and the view that follows it until it is stopped.
// Polling: every few seconds while the server moves between states on its own
// (requested to running, and a stop), backing off to a minute while it runs,
// none once it has ended; a hidden tab skips its polls and refreshes when it
// is shown again.
import { computed, nextTick, onMounted, onUnmounted, ref } from 'vue';
import { useRoute, useRouter } from 'vue-router';
import HostedServerStatus from '../components/HostedServerStatus.vue';
import HostedSettingsForm from '../components/HostedSettingsForm.vue';
import { fetchMyHostedServer, type HostedError } from '../api/hostedServer';
import { useSession } from '../composables/useSession';
import type { HostedServer, MyHostedServerResponse } from '../types/hostedServer';
import { isTransitioning } from '../utils/hostedState';

const { user, refresh: refreshSession } = useSession();
const route = useRoute();
const router = useRouter();

// The polls' periods: while the server changes state on its own, while it
// runs (doubling from the first to the last), and after a failed poll
// (doubling likewise).
const TRANSITION_POLL_MS = 3_000;
const RUNNING_POLL_FIRST_MS = 10_000;
const RUNNING_POLL_MAX_MS = 60_000;
const RETRY_POLL_FIRST_MS = 5_000;
const RETRY_POLL_MAX_MS = 60_000;

// Null until the first load answered.
const data = ref<MyHostedServerResponse | null>(null);
const loading = ref(false);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const pageStatus = ref('');
const pageError = ref('');
const pollTrouble = ref(false);
const pageHeading = ref<HTMLElement | null>(null);
const statusCard = ref<{ focusHeading: () => void } | null>(null);
// A new form (fresh from the last server's settings) after each server ends.
const formKey = ref(0);

const hostYourself =
  'You can still host a game yourself from the OpenNova game or with opennova-serve while logged in ' +
  'with this account: it is listed under your account on My servers.';

type View = 'loading' | 'error' | 'live' | 'off' | 'unapproved' | 'form';
const view = computed<View>(() => {
  const d = data.value;
  if (!d) return pageError.value && !loading.value ? 'error' : 'loading';
  if (d.server?.live) return 'live';
  if (!d.enabled) return 'off';
  if (!d.can_host) return 'unapproved';
  return 'form';
});

let pollTimer: ReturnType<typeof setTimeout> | undefined;
let clockTimer: ReturnType<typeof setInterval> | undefined;
let inFlight: AbortController | null = null;
let runningDelay = RUNNING_POLL_FIRST_MS;
let retryDelay = RETRY_POLL_FIRST_MS;
let lastState: string | null = null;
let unmounted = false;

function clearPoll() {
  if (pollTimer) clearTimeout(pollTimer);
  pollTimer = undefined;
}

// The next poll, by what the server is doing now.
function schedule(failed: boolean) {
  clearPoll();
  if (unmounted) return;
  let delay: number | null;
  if (failed) {
    delay = retryDelay;
    retryDelay = Math.min(retryDelay * 2, RETRY_POLL_MAX_MS);
  } else {
    retryDelay = RETRY_POLL_FIRST_MS;
    const server = data.value?.server ?? null;
    if (!server || !server.live) {
      delay = null;
    } else if (isTransitioning(server.state)) {
      delay = TRANSITION_POLL_MS;
    } else {
      delay = runningDelay;
      runningDelay = Math.min(runningDelay * 2, RUNNING_POLL_MAX_MS);
    }
  }
  if (delay === null) return;
  pollTimer = setTimeout(() => {
    pollTimer = undefined;
    // A hidden tab waits for visibilitychange, which loads at once.
    if (typeof document !== 'undefined' && document.hidden) return;
    void load(true);
  }, delay);
}

// What a change of state says, and the backoff it restarts.
function noteState(server: HostedServer | null) {
  const state = server?.state ?? null;
  if (state === lastState) return;
  if (state === 'running') runningDelay = RUNNING_POLL_FIRST_MS;
  const was = lastState;
  lastState = state;
  if (was === null || !server) return;
  if (state === 'running') pageStatus.value = 'Your server is running and listed on NovaWorld.';
  else if (state === 'failed') pageStatus.value = 'Your server failed. The timeline below says what happened.';
  else if (state === 'stopped') pageStatus.value = 'Your server has stopped.';
  if (!server.live) formKey.value++;
}

async function onSessionEnded() {
  await refreshSession();
  await router.replace({ path: '/login', query: { redirect: route.fullPath } });
}

// `background` keeps what the page shows (and says nothing) on a failed poll;
// an asked-for refresh shows the failure.
async function load(background: boolean) {
  if (inFlight) {
    if (background) return;
    inFlight.abort();
  }
  const controller = new AbortController();
  inFlight = controller;
  if (!background) loading.value = true;
  const result = await fetchMyHostedServer(controller.signal);
  if (inFlight !== controller) return; // a newer load took over
  inFlight = null;
  loading.value = false;
  now.value = Date.now();
  if (result.ok) {
    data.value = result.value;
    loadedAt.value = now.value;
    pollTrouble.value = false;
    if (!background) pageError.value = '';
    noteState(result.value.server);
    schedule(false);
    return;
  }
  if (result.error.code === 'unauthorized') {
    await onSessionEnded();
    return;
  }
  if (background && data.value !== null) pollTrouble.value = true;
  else pageError.value = result.error.message;
  schedule(true);
}

// The start went in: the server shows at once as the service answered it,
// the keyboard goes to its heading, and the page follows it from there.
async function onStarted(server: HostedServer | null) {
  pageError.value = '';
  pageStatus.value = 'Start requested. Starting takes a few minutes; this page follows it.';
  if (server && data.value) {
    data.value = { ...data.value, server };
    lastState = server.state;
  }
  void load(true);
  await nextTick();
  if (statusCard.value) statusCard.value.focusHeading();
  else pageHeading.value?.focus();
}

function onStopped(server: HostedServer | null) {
  pageError.value = '';
  pageStatus.value = '';
  if (server && data.value) {
    // The stop's reply carries no timeline: keep the one shown.
    const events = data.value.server?.events;
    data.value = { ...data.value, server: { ...server, events } };
    noteState(server);
  }
  void load(true);
}

// A refusal that means this page's picture is out of date: read it again.
function onRefused(error: HostedError) {
  if (error.code === 'unauthorized') {
    void onSessionEnded();
    return;
  }
  if (
    error.code === 'already_live' ||
    error.code === 'not_approved' ||
    error.code === 'hosting_disabled' ||
    error.code === 'no_live_server'
  ) {
    void load(true);
  }
}

function onVisibility() {
  if (!document.hidden && !pollTimer && data.value?.server?.live) void load(true);
}

onMounted(() => {
  void load(false);
  clockTimer = setInterval(() => {
    now.value = Date.now();
  }, 15_000);
  document.addEventListener('visibilitychange', onVisibility);
});

onUnmounted(() => {
  unmounted = true;
  clearPoll();
  if (clockTimer) clearInterval(clockTimer);
  document.removeEventListener('visibilitychange', onVisibility);
  // Its answer, when it comes, then finds a newer load in charge and is dropped.
  inFlight?.abort();
  inFlight = null;
});
</script>
