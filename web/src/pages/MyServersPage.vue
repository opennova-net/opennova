<template>
  <section class="px-4 pt-24 pb-16 sm:px-6">
    <div class="mx-auto max-w-6xl space-y-8">
      <header class="flex flex-col gap-4 sm:flex-row sm:items-end sm:justify-between">
        <div>
          <p class="text-sm uppercase tracking-[0.3em] text-accent">NovaWorld</p>
          <h1 ref="heading" tabindex="-1" class="text-3xl font-semibold text-ink focus:outline-none">My servers</h1>
          <p class="mt-1 text-sm text-ink-muted">
            The servers you run on NovaWorld as
            <span class="font-medium text-ink">{{ user?.username ?? 'your account' }}</span>.
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
            @click="refresh"
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
      <p v-if="pollTrouble && view !== 'hidden'" class="text-xs text-warn">
        The last refresh did not get through; this page keeps trying.
      </p>

      <div
        v-if="firstLoading"
        class="animate-pulse rounded-panel border border-border bg-panel p-8 text-center text-ink-muted"
      >
        Loading your servers…
      </div>

      <template v-else>
        <!-- The hosted server (GET /api/me/server): an approved account's one
             Start, or its live server with its one Stop. A first read that
             failed shows nothing here (the account may not be approved): the
             page's alert says it, and the read is retried. -->
        <section
          v-if="view === 'live' || view === 'off' || view === 'start'"
          class="space-y-8"
          aria-label="Your hosted server"
        >
          <template v-if="view === 'live' && hosted?.server">
            <p v-if="!hosted.enabled" class="rounded-panel border border-warn/40 bg-warn/10 p-4 text-sm text-warn">
              Hosting is switched off right now, so your server may stop working.
            </p>
            <p v-if="!hosted.can_host" class="rounded-panel border border-warn/40 bg-warn/10 p-4 text-sm text-warn">
              Your account can no longer host, so this server is being stopped.
            </p>
            <HostedServerStatus
              ref="statusCard"
              :server="hosted.server"
              :listing="listings.hostedListing"
              :now="now"
              @stopped="onHostedStopped"
              @refused="onRefused"
              @console-gone="onConsoleGone"
              @session-ended="onSessionEnded"
              @listing-changed="onListingChanged"
            />
          </template>

          <div v-else-if="view === 'off'" class="space-y-3 rounded-panel border border-border bg-panel p-6">
            <h2 class="text-lg font-semibold text-ink">Hosting is off</h2>
            <p class="text-sm text-ink-muted">Hosted servers are switched off right now.</p>
          </div>

          <template v-else-if="view === 'start' && hosted">
            <HostedSettingsForm
              :key="formKey"
              :previous="hosted.server?.settings ?? null"
              :data-sets="hosted.data_sets"
              :retry="startKind(hosted.server) === 'retry'"
              @started="onStarted"
              @refused="onRefused"
            />
            <!-- The last one, stopped or failed. -->
            <HostedServerStatus v-if="hosted.server" :server="hosted.server" :now="now" />
          </template>
        </section>

        <!-- The servers the account hosts itself (GET /api/me/hosts). -->
        <section class="space-y-6" aria-labelledby="my-servers-hosted-yourself">
          <h2 id="my-servers-hosted-yourself" class="text-xl font-semibold text-ink">Games you host</h2>
          <p v-if="listings.others === null" class="text-sm text-ink-muted">
            Your games could not be loaded. Refresh to try again.
          </p>
          <p v-else-if="listings.others.length === 0" class="text-sm text-ink-muted">
            Games you host while logged in as
            <span class="font-medium text-ink">{{ user?.username ?? 'you' }}</span> show up here.
          </p>
          <template v-else>
            <MyServerCard
              v-for="server in listings.others"
              :key="server.rid"
              :server="server"
              :now="now"
              @changed="refreshListSoon"
              @stopped="onListingStopped"
              @session-ended="onSessionEnded"
            />
          </template>
        </section>
      </template>
    </div>
  </section>
</template>

<script setup lang="ts">
// /me/servers: everything the logged-in account runs. Its hosted server
// (GET /api/me/server): for an approved account, the form with the page's one
// start control while nothing is live, else the live server's state, listing,
// console and stop (HostedServerStatus). Then every server the account hosts
// itself (GET /api/me/hosts, MyServerCard); the hosted server's own listing
// shows only in its section (utils/myServersView.ts).
//
// Polling, each read its own: the hosted server every few seconds while it
// moves between states on its own (requested to running, and a stop), backing
// off to a minute while it runs, none once it has ended, but read again when
// the tab is shown and when the listings name a hosted server it does not
// show (one started elsewhere); the listings every LIST_POLL_SECONDS. A hidden
// tab skips both reads' polls and catches up when shown again. The running
// server's console (HostedServerConsole) makes its admitted calls through
// this page's one console queue, so no two of them overlap (the service runs
// one console request per account at once).
import { computed, nextTick, onMounted, onUnmounted, ref } from 'vue';
import { useRoute, useRouter } from 'vue-router';
import HostedServerStatus from '../components/HostedServerStatus.vue';
import HostedSettingsForm from '../components/HostedSettingsForm.vue';
import MyServerCard from '../components/MyServerCard.vue';
import { fetchMyHostedServer, type HostedError } from '../api/hostedServer';
import { listMyServers } from '../api/myServers';
import { provideConsoleQueue } from '../composables/useConsoleQueue';
import { useSession } from '../composables/useSession';
import type { HostedServer, MyHostedServerResponse } from '../types/hostedServer';
import type { MyServer } from '../types/myServers';
import { isTransitioning } from '../utils/hostedState';
import { hostedView, listingsOutrunHosted, splitListings, startKind } from '../utils/myServersView';

const { user, refresh: refreshSession } = useSession();
const route = useRoute();
const router = useRouter();
provideConsoleQueue();

// The hosted server's polls: while it changes state on its own, while it runs
// (doubling from the first to the last), and after a failed poll (doubling
// likewise).
const TRANSITION_POLL_MS = 3_000;
const RUNNING_POLL_FIRST_MS = 10_000;
const RUNNING_POLL_MAX_MS = 60_000;
const RETRY_POLL_FIRST_MS = 5_000;
const RETRY_POLL_MAX_MS = 60_000;
// The listings refresh themselves on this period (a server's columns and
// roster come with its own updates), and on demand.
const LIST_POLL_SECONDS = 10;

// Each null until its first read answered.
const hosted = ref<MyHostedServerResponse | null>(null);
const servers = ref<MyServer[] | null>(null);
const hostedLoading = ref(false);
const listLoading = ref(false);
const hostedError = ref('');
const listError = ref('');
// A start or stop the service refused for a reason the page then re-reads
// (the form or card that showed it goes with the re-read): kept here until
// the next refresh, start or stop.
const hostedNotice = ref('');
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const pageStatus = ref('');
const pollTrouble = ref(false);
const heading = ref<HTMLElement | null>(null);
const statusCard = ref<{ focusHeading: () => void } | null>(null);
// A new form (fresh from the last server's settings) after each server ends.
const formKey = ref(0);

const loading = computed(() => hostedLoading.value || listLoading.value);
const view = computed(() => hostedView(hosted.value, hostedError.value !== '' && !hostedLoading.value));
// Each message once (both reads can fail alike, with no connection); the
// hosted read's failures say nothing to an account without a hosted section.
const pageError = computed(() => {
  const messages = [hostedNotice.value, view.value === 'hidden' ? '' : hostedError.value, listError.value];
  return messages.filter((m, i) => m !== '' && messages.indexOf(m) === i).join(' ');
});
const listings = computed(() => splitListings(servers.value, hosted.value?.server ?? null));
const firstLoading = computed(
  () => view.value === 'loading' || (servers.value === null && (listError.value === '' || listLoading.value)),
);

async function onSessionEnded() {
  await refreshSession();
  await router.replace({ path: '/login', query: { redirect: route.fullPath } });
}

function refresh() {
  hostedNotice.value = '';
  void loadHosted(false);
  void loadList(false);
}

// ---------------------------------------------------------------------------
// The hosted server.

let hostedTimer: ReturnType<typeof setTimeout> | undefined;
let hostedSoonTimer: ReturnType<typeof setTimeout> | undefined;
let hostedInFlight: AbortController | null = null;
let runningDelay = RUNNING_POLL_FIRST_MS;
let retryDelay = RETRY_POLL_FIRST_MS;
let lastState: string | null = null;
let unmounted = false;

function clearHostedPoll() {
  if (hostedTimer) clearTimeout(hostedTimer);
  hostedTimer = undefined;
}

// The next poll, by what the server is doing now.
function scheduleHosted(failed: boolean) {
  clearHostedPoll();
  if (unmounted) return;
  let delay: number | null;
  if (failed) {
    delay = retryDelay;
    retryDelay = Math.min(retryDelay * 2, RETRY_POLL_MAX_MS);
  } else {
    retryDelay = RETRY_POLL_FIRST_MS;
    const server = hosted.value?.server ?? null;
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
  hostedTimer = setTimeout(() => {
    hostedTimer = undefined;
    // A hidden tab waits for visibilitychange, which loads at once.
    if (typeof document !== 'undefined' && document.hidden) return;
    void loadHosted(true);
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
  if (state === 'running') pageStatus.value = 'Your server is up. Players can join it now.';
  else if (state === 'failed') pageStatus.value = 'Your server failed. You can try again.';
  else if (state === 'stopped') pageStatus.value = 'Your server has stopped.';
  if (!server.live) formKey.value++;
}

// `background` keeps what the page shows (and says nothing) on a failed poll;
// an asked-for refresh shows the failure.
async function loadHosted(background: boolean) {
  if (hostedInFlight) {
    if (background) return;
    hostedInFlight.abort();
  }
  const controller = new AbortController();
  hostedInFlight = controller;
  if (!background) hostedLoading.value = true;
  const result = await fetchMyHostedServer(controller.signal);
  if (hostedInFlight !== controller) return; // a newer load took over
  hostedInFlight = null;
  hostedLoading.value = false;
  now.value = Date.now();
  if (result.ok) {
    hosted.value = result.value;
    loadedAt.value = now.value;
    pollTrouble.value = false;
    // Any answer clears a failure the page showed (a background retry
    // included); a silent poll failure only ever set pollTrouble.
    hostedError.value = '';
    noteState(result.value.server);
    scheduleHosted(false);
    return;
  }
  if (result.error.code === 'unauthorized') {
    await onSessionEnded();
    return;
  }
  if (background && hosted.value !== null) pollTrouble.value = true;
  else hostedError.value = result.error.message;
  scheduleHosted(true);
}

// A read that started before the page's own change answers with the state
// from before it: drop it, so the read that follows the change is the one in
// charge.
function dropHostedInFlight() {
  hostedInFlight?.abort();
  hostedInFlight = null;
  hostedLoading.value = false;
}

// The start went in: the server shows at once as the service answered it (the
// form, and with it the start, goes), the keyboard goes to its heading, and
// the page follows it from there.
async function onStarted(server: HostedServer | null) {
  hostedError.value = '';
  hostedNotice.value = '';
  pageStatus.value = 'Your server is starting.';
  if (server && hosted.value) {
    hosted.value = { ...hosted.value, server };
    lastState = server.state;
  }
  dropHostedInFlight();
  void loadHosted(true);
  await nextTick();
  if (statusCard.value) statusCard.value.focusHeading();
  else heading.value?.focus();
}

function onHostedStopped(server: HostedServer | null) {
  hostedError.value = '';
  hostedNotice.value = '';
  pageStatus.value = '';
  if (server && hosted.value) {
    // The stop's reply carries no timeline: keep the one shown.
    const events = hosted.value.server?.events;
    hosted.value = { ...hosted.value, server: { ...server, events } };
    noteState(server);
  }
  dropHostedInFlight();
  void loadHosted(true);
  refreshListSoon();
}

// A refusal that means this page's picture is out of date: read it again.
// The re-read takes the form away (already_live), or the whole section
// (not_approved), so the page keeps the refusal's sentence.
function onRefused(error: HostedError) {
  if (error.code === 'unauthorized') {
    void onSessionEnded();
    return;
  }
  if (error.code === 'already_live' || error.code === 'not_approved') hostedNotice.value = error.message;
  if (
    error.code === 'already_live' ||
    error.code === 'not_approved' ||
    error.code === 'hosting_disabled' ||
    error.code === 'no_live_server'
  ) {
    dropHostedInFlight();
    void loadHosted(true);
  }
}

// The console found the server no longer running (it is stopping, or it
// crashed and is starting again): read it now rather than at the next poll.
function onConsoleGone() {
  dropHostedInFlight();
  void loadHosted(true);
}

// A shown tab reads the hosted server at once unless a poll is already
// due (one that came due while it was hidden did not read), and the listings
// when a poll was skipped meanwhile.
function onVisibility() {
  if (document.hidden) return;
  if (!hostedTimer) void loadHosted(true);
  if (listMissed) {
    listMissed = false;
    void loadList(true);
  }
}

// A push to the hosted server's listing: its name and the like come back
// with the server's next update, so look at both reads again shortly.
function onListingChanged() {
  refreshListSoon();
  if (hostedSoonTimer) clearTimeout(hostedSoonTimer);
  hostedSoonTimer = setTimeout(() => {
    hostedSoonTimer = undefined;
    void loadHosted(true);
  }, 1500);
}

// ---------------------------------------------------------------------------
// The servers the account hosts itself.

let listTimer: ReturnType<typeof setInterval> | undefined;
let listSoonTimer: ReturnType<typeof setTimeout> | undefined;
let listInFlight: AbortController | null = null;
// A poll came due while the tab was hidden: the tab reads the list when shown.
let listMissed = false;

// `background` keeps the list (and says nothing) on a failed poll; an
// asked-for refresh shows the failure.
async function loadList(background: boolean) {
  if (listInFlight) {
    if (background) return;
    listInFlight.abort();
  }
  const controller = new AbortController();
  listInFlight = controller;
  if (!background) listLoading.value = true;
  const result = await listMyServers(controller.signal);
  if (listInFlight !== controller) return; // a newer load took over
  listInFlight = null;
  listLoading.value = false;
  now.value = Date.now();
  if (result.ok) {
    // An asked-for refresh, or the first list after none, clears a failure
    // shown (the failures that set one).
    if (!background || servers.value === null) listError.value = '';
    servers.value = result.value;
    loadedAt.value = now.value;
    // A hosted server this page does not show is live (started from another
    // tab or device): the hosted read is stale, so its start would be too.
    if (listingsOutrunHosted(result.value, hosted.value?.server ?? null)) void loadHosted(true);
    return;
  }
  if (result.error.code === 'unauthorized') {
    await onSessionEnded();
    return;
  }
  if (!background || servers.value === null) listError.value = result.error.message;
}

// After a push: the server's own update carries most changes, so look again
// shortly.
function refreshListSoon() {
  if (listSoonTimer) clearTimeout(listSoonTimer);
  listSoonTimer = setTimeout(() => void loadList(true), 1500);
}

// The stop went out: the card goes at once (the service takes the row out as
// the statement is sent, which a list read right now could still beat), a
// list read already in flight is dropped for the same reason, the list is read
// again shortly, and the keyboard goes to the page heading, the confirm form it
// was on having gone with the card; the status line says what happened.
async function onListingStopped(rid: number, name: string) {
  pageStatus.value = `Stopped hosting ${name || 'the server'}. Its match ends for everyone on it.`;
  listError.value = '';
  if (servers.value !== null) servers.value = servers.value.filter((s) => s.rid !== rid);
  listInFlight?.abort();
  listInFlight = null;
  listLoading.value = false;
  refreshListSoon();
  await nextTick();
  heading.value?.focus();
}

onMounted(() => {
  refresh();
  listTimer = setInterval(() => {
    now.value = Date.now();
    if (document.hidden) {
      listMissed = true;
      return;
    }
    void loadList(true);
  }, LIST_POLL_SECONDS * 1000);
  document.addEventListener('visibilitychange', onVisibility);
});

onUnmounted(() => {
  unmounted = true;
  clearHostedPoll();
  if (hostedSoonTimer) clearTimeout(hostedSoonTimer);
  if (listTimer) clearInterval(listTimer);
  if (listSoonTimer) clearTimeout(listSoonTimer);
  document.removeEventListener('visibilitychange', onVisibility);
  // Their answers, when they come, then find a newer load in charge and are
  // dropped.
  hostedInFlight?.abort();
  hostedInFlight = null;
  listInFlight?.abort();
  listInFlight = null;
});
</script>
