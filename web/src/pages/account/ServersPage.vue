<template>
  <div class="space-y-8">
    <header class="flex flex-col gap-4 sm:flex-row sm:items-end sm:justify-between">
      <div>
        <h1 ref="heading" tabindex="-1" class="text-3xl font-semibold text-ink focus:outline-none">Servers</h1>
        <p class="mt-1 text-sm text-ink-muted">
          The servers NovaWorld hosts for you, and the games you host yourself as
          <span class="font-medium text-ink">{{ user?.username ?? 'your account' }}</span>.
        </p>
      </div>
      <div class="flex items-center gap-3">
        <p v-if="loadedAt" class="text-xs text-ink-muted">Updated <RelativeTime :value="loadedAt" :now="now" /></p>
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
    <p v-if="pollTrouble" class="text-xs text-warn">The last refresh did not get through; this page keeps trying.</p>

    <div v-if="view === 'loading'" class="animate-pulse rounded-panel border border-border bg-panel p-8 text-center text-ink-muted">
      Loading your servers…
    </div>

    <div v-else-if="view === 'error'" class="space-y-3 rounded-panel border border-border bg-panel p-6">
      <h2 class="text-lg font-semibold text-ink">Your hosted servers could not be loaded</h2>
      <p class="text-sm text-ink-muted">This page tries again by itself, or refresh it now.</p>
    </div>

    <template v-else-if="data">
      <!-- The hosting area: the allowance and the start, the support tier
           callout, or hosting switched off. -->
      <HostingLockedCallout v-if="view === 'locked'" id="servers-locked" />

      <section v-else-if="view === 'off'" class="space-y-2 rounded-panel border border-border bg-panel p-6" aria-labelledby="servers-off-heading">
        <h2 id="servers-off-heading" class="text-lg font-semibold text-ink">Hosting is off</h2>
        <p class="text-sm text-ink-muted">NovaWorld is not starting hosted servers right now. Servers already live keep running.</p>
      </section>

      <section v-else class="space-y-4 rounded-panel border border-border bg-panel p-4 sm:p-6" aria-labelledby="servers-allowance-heading">
        <h2 id="servers-allowance-heading" class="sr-only">Your hosting allowance</h2>
        <div class="grid gap-6 sm:grid-cols-2">
          <div class="space-y-1">
            <AllowanceMeter id="servers-allowance" :used="data.entitlement.used" :max="data.entitlement.max_servers" />
            <p class="text-xs text-ink-muted">{{ sourceLine }}</p>
          </div>
          <AllowanceMeter
            v-if="data.entitlement.own_machines > 0"
            id="servers-own-machines"
            :used="data.entitlement.own_used"
            :max="data.entitlement.own_machines"
            singular="own machine"
            plural="own machines"
          />
        </div>
        <div class="flex flex-col gap-3 border-t border-border pt-4 sm:flex-row sm:items-center sm:justify-between">
          <p v-if="blockText" class="text-sm" :class="block === 'past_due' ? 'text-warn' : 'text-ink-muted'">
            {{ blockText }}
            <RouterLink v-if="block === 'past_due'" to="/me/support" class="text-accent hover:text-ink-bright">
              Update your payment method
            </RouterLink>
            <RouterLink
              v-else-if="block === 'limit' && data.entitlement.source !== 'comp'"
              to="/me/support"
              class="text-accent hover:text-ink-bright"
            >
              See the support tiers
            </RouterLink>
          </p>
          <p v-else class="text-sm text-ink-muted">Hosted servers are experimental (pre-1.0).</p>
          <RouterLink
            v-if="block === null"
            to="/me/servers/new"
            aria-describedby="servers-allowance"
            class="shrink-0 rounded-control bg-accent px-5 py-2.5 text-center text-sm font-semibold text-on-accent transition hover:bg-accent/90"
          >
            Start a server
          </RouterLink>
        </div>
      </section>

      <!-- The account's hosted servers: the live ones, then the newest ended. -->
      <section v-if="live.length > 0" class="space-y-4" aria-labelledby="servers-live-heading">
        <h2 id="servers-live-heading" class="text-xl font-semibold text-ink">Live servers</h2>
        <HostedServerCard
          v-for="server in live"
          :key="server.id"
          :server="server"
          :now="now"
          :listing="listings.hosted.get(server.id) ?? null"
          :can-start="block === null"
        />
      </section>

      <section v-if="ended.length > 0" class="space-y-4" aria-labelledby="servers-ended-heading">
        <h2 id="servers-ended-heading" class="text-xl font-semibold text-ink">Recently ended</h2>
        <HostedServerCard
          v-for="server in ended"
          :key="server.id"
          :server="server"
          :now="now"
          :can-start="block === null"
        />
      </section>
    </template>

    <!-- The servers the account hosts itself (GET /api/me/hosts). -->
    <section class="space-y-6" aria-labelledby="servers-hosted-yourself">
      <h2 id="servers-hosted-yourself" class="text-xl font-semibold text-ink">Games you host</h2>
      <p v-if="listings.others === null" class="text-sm text-ink-muted">
        {{ listLoading || (hosts === null && !listError) ? 'Loading your games…' : 'Your games could not be loaded. Refresh to try again.' }}
      </p>
      <p v-else-if="listings.others.length === 0" class="text-sm text-ink-muted">
        Games you host from the game while logged in as
        <span class="font-medium text-ink">{{ user?.username ?? 'you' }}</span> show up here.
      </p>
      <template v-else>
        <MyServerCard
          v-for="server in listings.others"
          :key="server.rid"
          :server="server"
          :now="now"
          @changed="listSoon"
          @stopped="onListingStopped"
          @session-ended="onSessionEnded"
        />
      </template>
    </section>
  </div>
</template>

<script setup lang="ts">
// /me/servers: everything the logged-in account runs. From GET /api/me/servers
// its hosting allowance (the AllowanceMeter, "2 of 3 servers used") with the
// start (to /me/servers/new) while it has room, the support tier callout
// while it has none, and its hosted servers (HostedServerCard, each linking
// to its own page); from GET /api/me/hosts the games it hosts itself
// (MyServerCard; a hosted server's own listing shows on its card instead,
// utils/myServersView.ts).
//
// Polling (usePolling), each read its own: the hosted servers every 3 s while
// any changes state on its own, backing off from 10 s to a minute while they
// run, none once nothing is live (read again when the tab is shown, and when
// the listings name a hosted server this page does not show as live); the
// listings every LIST_POLL_MS.
import { computed, nextTick, onUnmounted, ref } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import AllowanceMeter from '../../components/AllowanceMeter.vue';
import HostedServerCard from '../../components/HostedServerCard.vue';
import HostingLockedCallout from '../../components/HostingLockedCallout.vue';
import MyServerCard from '../../components/MyServerCard.vue';
import RelativeTime from '../../components/RelativeTime.vue';
import { STOPPING_STILL_COUNTS, fetchMyServers } from '../../api/hostedServer';
import { listMyServers } from '../../api/myServers';
import { hostedCadence, usePolling } from '../../composables/usePolling';
import { useSession } from '../../composables/useSession';
import type { HostedServer, HostedServersResponse } from '../../types/hostedServer';
import type { MyServer } from '../../types/myServers';
import {
  endedServers,
  hostingView,
  listingsOutrunHosted,
  liveServers,
  splitListings,
  startBlock,
} from '../../utils/myServersView';

// The listings refresh themselves on this period (a server's columns and
// roster come with its own updates), and on demand.
const LIST_POLL_MS = 10_000;

const { user, refresh: refreshSession } = useSession();
const route = useRoute();
const router = useRouter();

// Each null until its first read answered.
const data = ref<HostedServersResponse | null>(null);
const hosts = ref<MyServer[] | null>(null);
const hostedError = ref('');
const listError = ref('');
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const pageStatus = ref('');
const pollTrouble = ref(false);
const heading = ref<HTMLElement | null>(null);

const view = computed(() => hostingView(data.value, hostedError.value !== ''));
const block = computed(() => (data.value ? startBlock(data.value) : null));
const live = computed(() => liveServers(data.value?.servers ?? []));
const ended = computed(() => endedServers(data.value?.servers ?? []));
const listings = computed(() => splitListings(hosts.value, data.value?.servers ?? []));

const sourceLine = computed(() => {
  switch (data.value?.entitlement.source) {
    case 'tier':
      return 'From your support tier.';
    case 'comp':
      return 'Granted to your account by an admin.';
    default:
      return '';
  }
});

const blockText = computed(() => {
  const e = data.value?.entitlement;
  switch (block.value) {
    case 'past_due':
      return "Your support tier's last payment did not go through. Your servers keep running while Stripe retries the card; new servers can't start.";
    case 'limit': {
      const stopping = live.value.some((s) => s.state === 'stopping');
      const all =
        e?.max_servers === 1 ? 'The one server you may run is live.' : `All ${e?.max_servers} servers you may run are live.`;
      return stopping
        ? `${all} ${STOPPING_STILL_COUNTS}`
        : `${all} Stop one to start another${e?.source === 'comp' ? '.' : ', or move to a bigger support tier.'}`;
    }
    case 'full':
      return 'Every hosted server place on NovaWorld is taken right now. Try again later.';
    default:
      return '';
  }
});

// Each message once (both reads can fail alike, with no connection).
const pageError = computed(() => {
  const messages = [hostedError.value, listError.value];
  return messages.filter((m, i) => m !== '' && messages.indexOf(m) === i).join(' ');
});

async function onSessionEnded() {
  await refreshSession();
  await router.replace({ path: '/login', query: { redirect: route.fullPath } });
}

// What a change of state says: each server that came up, failed or stopped
// since the last read.
function noteChanges(before: readonly HostedServer[] | undefined, after: readonly HostedServer[]) {
  if (!before) return;
  const was = new Map(before.map((s) => [s.id, s.state]));
  const lines: string[] = [];
  for (const s of after) {
    const old = was.get(s.id);
    if (old === undefined || old === s.state) continue;
    const name = s.settings.game_name || 'Your server';
    if (s.state === 'running') lines.push(`${name} is up. Players can join it now.`);
    else if (s.state === 'failed') lines.push(`${name} failed.`);
    else if (s.state === 'stopped') lines.push(`${name} has stopped.`);
  }
  if (lines.length > 0) pageStatus.value = lines.join(' ');
}

// ---------------------------------------------------------------------------
// The hosted servers.

const cadence = hostedCadence();

const hostedPoll = usePolling({
  async read(signal, background) {
    const result = await fetchMyServers(signal);
    if (signal.aborted) return false;
    now.value = Date.now();
    if (result.ok) {
      noteChanges(data.value?.servers, result.value.servers);
      data.value = result.value;
      loadedAt.value = now.value;
      pollTrouble.value = false;
      hostedError.value = '';
      return true;
    }
    if (result.error.code === 'unauthorized') {
      await onSessionEnded();
      return false;
    }
    if (background && data.value !== null) pollTrouble.value = true;
    else hostedError.value = result.error.message;
    return false;
  },
  next: () => cadence(data.value?.servers ?? []),
});

// ---------------------------------------------------------------------------
// The servers the account hosts itself.

const listPoll = usePolling({
  async read(signal, background) {
    const result = await listMyServers(signal);
    if (signal.aborted) return false;
    now.value = Date.now();
    if (result.ok) {
      // An asked-for refresh, or the first list after none, clears a failure
      // shown.
      if (!background || hosts.value === null) listError.value = '';
      hosts.value = result.value;
      // A hosted server this page does not show as live (started from
      // another tab or device): the hosted read is stale.
      if (data.value && listingsOutrunHosted(result.value, data.value.servers)) void hostedPoll.poke();
      return true;
    }
    if (result.error.code === 'unauthorized') {
      await onSessionEnded();
      return false;
    }
    if (!background || hosts.value === null) listError.value = result.error.message;
    return false;
  },
  next: () => LIST_POLL_MS,
});

const listLoading = listPoll.loading;
const loading = computed(() => hostedPoll.loading.value || listPoll.loading.value);

function refresh() {
  pageStatus.value = '';
  void hostedPoll.refresh();
  void listPoll.refresh();
}

// After a push: the server's own update carries most changes, so look again
// shortly.
let listSoonTimer: ReturnType<typeof setTimeout> | undefined;
function listSoon(again: () => Promise<void> = listPoll.poke) {
  if (listSoonTimer) clearTimeout(listSoonTimer);
  listSoonTimer = setTimeout(() => void again(), 1500);
}
onUnmounted(() => {
  if (listSoonTimer) clearTimeout(listSoonTimer);
});

// The stop went out: the card goes at once (the service takes the row out as
// the statement is sent, which a list read right now could still beat), the
// list is read again shortly (a read in flight dropped, for the same reason),
// and the keyboard goes to the page heading, the confirm form it was on
// having gone with the card.
async function onListingStopped(rid: number, name: string) {
  pageStatus.value = `Stopped hosting ${name || 'the server'}. Its match ends for everyone on it.`;
  listError.value = '';
  if (hosts.value !== null) hosts.value = hosts.value.filter((s) => s.rid !== rid);
  listSoon(listPoll.readAgain);
  await nextTick();
  heading.value?.focus();
}
</script>
