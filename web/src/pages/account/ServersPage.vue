<template>
  <div class="space-y-8">
    <PageHeader title="Servers" refreshable :loaded-at="loadedAt" :now="now" :loading="loading" @refresh="refresh">
      <template #actions>
        <RouterLink v-if="block === null" to="/me/servers/new" class="btn-primary" :aria-describedby="meterIds">Start a server</RouterLink>
      </template>
    </PageHeader>

    <LiveNotices :message="pageStatus" :error="pageError" :poll-trouble="pollTrouble" />

    <p v-if="view === 'loading'" class="text-sm text-ink-muted">Loading…</p>

    <template v-else-if="data">
      <HostingLockedCallout v-if="view === 'locked'" id="servers-locked" />

      <Notice v-else-if="view === 'off'" class="servers-off">Hosting is off right now.</Notice>

      <template v-else>
        <div v-if="data.entitlement.shared_servers > 0 || data.entitlement.own_machines > 0" class="servers-allowance card flex flex-col gap-3 p-4 sm:flex-row sm:gap-8">
          <AllowanceMeter
            v-if="data.entitlement.shared_servers > 0"
            id="servers-allowance"
            class="flex-1"
            :used="data.entitlement.shared_used"
            :max="data.entitlement.shared_servers"
            singular="shared server"
            plural="shared servers"
            :title="sourceTitle"
          />
          <AllowanceMeter
            v-if="data.entitlement.own_machines > 0"
            id="servers-own-machines"
            class="flex-1"
            :used="data.entitlement.own_used"
            :max="data.entitlement.own_machines"
            singular="dedicated machine"
            plural="dedicated machines"
          />
        </div>
        <Notice v-if="block !== null" :tone="block === 'past_due' ? 'warn' : 'neutral'" class="servers-block">
          {{ blockLine(block, stopping) }}
          <RouterLink v-if="link" :to="link.to" class="link">{{ link.label }}</RouterLink>
        </Notice>
      </template>

      <section v-if="live.length > 0" class="space-y-3" aria-labelledby="servers-live-heading">
        <h2 id="servers-live-heading" class="section-heading">Live</h2>
        <Notice v-if="launchesHeld" tone="warn" class="launches-held">New machines are held for a moment; waiting servers start when they resume.</Notice>
        <HostedServerCard
          v-for="server in live"
          :key="server.id"
          :server="server"
          :now="now"
          :listing="listings.hosted.get(server.id) ?? null"
          :can-start="block === null"
        />
      </section>

      <details v-if="ended.length > 0" class="servers-ended space-y-3">
        <summary class="section-heading">Recently ended ({{ endedShown.length }})</summary>
        <div class="mt-3 space-y-3">
          <HistoryToggle v-model="showEndedHistory" :count="replacedEndedIds.size" />
          <p v-if="endedShown.length === 0" class="text-sm text-ink-muted">All started again since.</p>
          <HostedServerCard v-for="server in endedShown" :key="server.id" :server="server" :now="now" :can-start="block === null" />
        </div>
      </details>

      <StatsDashboard
        v-if="data.servers.length > 0"
        base-id="servers-stats"
        heading="Your account's stats"
        variant="aggregate"
        :follow="live.length > 0"
        :load="fetchAccountStats"
        @session-ended="onSessionEnded"
      />
    </template>

    <!-- The servers the account hosts itself (GET /api/me/hosts). -->
    <section class="space-y-3" aria-labelledby="servers-hosted-yourself">
      <h2 id="servers-hosted-yourself" ref="hostedHeading" tabindex="-1" class="section-heading focus:outline-none">Games you host</h2>
      <p v-if="listings.others === null" class="text-sm text-ink-muted">
        {{ listLoading || (hosts === null && !listError) ? 'Loading…' : 'Not loaded.' }}
      </p>
      <p v-else-if="listings.others.length === 0" class="text-sm text-ink-muted">None right now.</p>
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
// its allowance (shared servers and dedicated machines) with the start while
// either has room, the way to hosting while it has none, its hosted servers
// (live, then recently ended) and their stats together (GET
// /api/me/servers/stats); from GET /api/me/hosts the games it hosts itself
// (MyServerCard; a hosted server's own listing shows on its card instead).
// The hosted servers poll on hostedCadence, the listings every LIST_POLL_MS.
import { computed, nextTick, onUnmounted, ref } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import AllowanceMeter from '../../components/AllowanceMeter.vue';
import HistoryToggle from '../../components/HistoryToggle.vue';
import HostedServerCard from '../../components/HostedServerCard.vue';
import HostingLockedCallout from '../../components/HostingLockedCallout.vue';
import MyServerCard from '../../components/MyServerCard.vue';
import StatsDashboard from '../../components/StatsDashboard.vue';
import LiveNotices from '../../components/ui/LiveNotices.vue';
import Notice from '../../components/ui/Notice.vue';
import PageHeader from '../../components/ui/PageHeader.vue';
import { fetchMyServers } from '../../api/hostedServer';
import { fetchAccountStats } from '../../api/hostedStats';
import { listMyServers } from '../../api/myServers';
import { hostedCadence, usePolling } from '../../composables/usePolling';
import { useSession } from '../../composables/useSession';
import type { HostedServer, HostedServersResponse } from '../../types/hostedServer';
import type { MyServer } from '../../types/myServers';
import { replacedServers } from '../../utils/history';
import {
  blockLine,
  blockLink,
  endedServers,
  hostingView,
  listingsOutrunHosted,
  liveServers,
  splitListings,
  startBlock,
} from '../../utils/myServersView';

// The listings refresh themselves on this period, and on demand.
const LIST_POLL_MS = 10_000;

const { refresh: refreshSession } = useSession();
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
const hostedHeading = ref<HTMLElement | null>(null);

const view = computed(() => hostingView(data.value, hostedError.value !== ''));
const block = computed(() => (data.value ? startBlock(data.value) : null));
const link = computed(() => (data.value && block.value ? blockLink(block.value, data.value.entitlement) : null));
const live = computed(() => liveServers(data.value?.servers ?? []));
const stopping = computed(() => live.value.some((s) => s.state === 'stopping'));
// The fleet's launches are held while one of them waits for a machine.
const launchesHeld = computed(() => live.value.some((s) => s.phase?.name === 'launches_held'));
const ended = computed(() => endedServers(data.value?.servers ?? []));
// An ended server started again since (one of the same name after it) is
// history, shown on request (utils/history.ts).
const showEndedHistory = ref(false);
const replacedEndedIds = computed(() => replacedServers(data.value?.servers ?? []));
const endedShown = computed(() => ended.value.filter((s) => showEndedHistory.value || !replacedEndedIds.value.has(s.id)));
const listings = computed(() => splitListings(hosts.value, data.value?.servers ?? []));

// Where the servers on shared machines come from, as the meter's tooltip.
const sourceTitle = computed(() => {
  switch (data.value?.entitlement.source) {
    case 'tier':
      return 'From your tier';
    case 'comp':
      return 'Granted by an admin';
    default:
      return undefined;
  }
});
// The meters the start link is described by: the ones shown.
const meterIds = computed(() => {
  const e = data.value?.entitlement;
  if (!e) return undefined;
  const ids = [...(e.shared_servers > 0 ? ['servers-allowance'] : []), ...(e.own_machines > 0 ? ['servers-own-machines'] : [])];
  return ids.length > 0 ? ids.join(' ') : undefined;
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
    if (s.state === 'running') lines.push(`${name} is up.`);
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

// The stop went out: the card goes at once (a list read right now could still
// show it), the list is read again shortly, and the keyboard goes to the
// section's heading, the card it was on having gone.
async function onListingStopped(rid: number, name: string) {
  pageStatus.value = `Stopped hosting ${name || 'the server'}.`;
  listError.value = '';
  if (hosts.value !== null) hosts.value = hosts.value.filter((s) => s.rid !== rid);
  listSoon(listPoll.readAgain);
  await nextTick();
  hostedHeading.value?.focus();
}
</script>
