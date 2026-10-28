<template>
  <div class="space-y-6">
    <PageHeader title="Servers" :tabs="MY_SERVERS_TABS" refreshable :loaded-at="loadedAt" :now="now" :loading="loading" @refresh="refresh">
      <template v-if="view === 'ready' && allowance.length > 0" #meta>
        <template v-for="(part, i) in allowance" :key="part.key">
          <span v-if="i > 0" aria-hidden="true">·</span>
          <span
            :id="`servers-allowance-${part.key}`"
            class="servers-allowance"
            :class="part.over ? 'text-danger' : undefined"
            :title="part.over ? OVER_TEXT : part.key === 'shared' ? source : undefined"
          >
            {{ part.text }}
          </span>
        </template>
      </template>
      <template #actions>
        <RouterLink v-if="canStart" to="/me/servers/new" class="btn-primary" :aria-describedby="allowanceIds">Start a server</RouterLink>
      </template>
    </PageHeader>

    <LiveNotices :message="pageStatus" :error="pageError" :poll-trouble="pollTrouble" />

    <template v-if="data">
      <HostingLockedCallout v-if="view === 'locked'" id="servers-locked" />
      <Notice v-else-if="view === 'off'" class="servers-off">Hosting is off right now.</Notice>
      <Notice v-else-if="block !== null" :tone="block === 'past_due' ? 'warn' : 'neutral'" class="servers-block">
        {{ blockLine(block, stopping) }}
        <RouterLink v-if="link" :to="link.to" class="link">{{ link.label }}</RouterLink>
      </Notice>
      <Notice v-if="launchesHeld" tone="warn" class="launches-held">New machines are held for a moment; waiting servers start when they resume.</Notice>
    </template>

    <div class="servers-filters flex flex-wrap items-center gap-x-4 gap-y-2">
      <div role="group" aria-label="State" class="flex gap-1">
        <RouterLink
          v-for="c in STATE_CHIPS"
          :key="c.value"
          :to="{ path: route.path, query: { ...route.query, state: c.value === 'live' ? undefined : c.value } }"
          :class="state === c.value ? 'btn-secondary btn-sm' : 'btn-ghost btn-sm'"
          :aria-current="state === c.value ? 'true' : undefined"
        >
          {{ c.label }}
        </RouterLink>
      </div>
      <HistoryToggle v-if="state !== 'live'" v-model="showHistory" :count="replacedIds.size" />
    </div>

    <div ref="tableBox" tabindex="-1" class="table-wrap focus:outline-none">
      <table class="data-table">
        <caption class="sr-only">Your servers</caption>
        <thead>
          <tr>
            <th scope="col">Server</th>
            <th scope="col">Map</th>
            <th scope="col">Players</th>
            <th scope="col">Runs on</th>
            <th scope="col">State</th>
            <th scope="col">Since</th>
            <th scope="col"><span class="sr-only">Actions</span></th>
          </tr>
        </thead>
        <tbody>
          <TableState v-if="rows.length === 0" :colspan="7" :rows="data === null && hosts === null ? null : rows" :loading="loading" :empty="emptyText" />
          <tr v-for="r in rows" :key="r.key" class="server-row" :class="r.server ? 'hosted-row' : 'listed-row'">
            <th scope="row" class="min-w-[12rem]">
              <ServerLink :name="r.name" :id="r.server?.id ?? null" :to="r.server ? null : `/me/servers/hosts/${r.listing?.rid}`" scope="owner" />
              <span v-if="r.game || r.gameType" class="server-game block text-xs font-normal text-ink-muted">
                {{ r.game }}<template v-if="r.game && r.gameType"> · </template><GameText v-if="r.gameType" :text="r.gameType" />
              </span>
            </th>
            <td class="server-map"><GameText v-if="r.map" :text="r.map" /><span v-else class="text-ink-muted">—</span></td>
            <td class="whitespace-nowrap">{{ r.players || '—' }}</td>
            <td class="server-runs-on whitespace-nowrap">
              <template v-if="r.server">
                {{ r.server.own_machine ? 'Dedicated machine' : 'Shared machine' }}
                <span v-if="r.server.address" class="block text-xs text-ink-muted">{{ r.server.address.name }}</span>
              </template>
              <template v-else>Your game</template>
            </td>
            <td class="server-state">
              <template v-if="r.server">
                <span :title="r.server.stopped_because && !r.server.live ? stoppedBecauseText(r.server.stopped_because, 'owner') : undefined">
                  <Badge :tone="serverStateTone(r.server.state)">{{ ownerStateLabel(r.server.state) }}</Badge>
                </span>
                <span v-if="phaseLine(r.server)" class="server-phase block max-w-xs text-xs" :class="phaseSlow(r.server, now) ? 'text-warn' : 'text-ink-muted'">
                  {{ phaseLine(r.server) }}
                </span>
                <span v-if="r.server.state === 'failed' && r.server.failure_reason" class="block max-w-xs break-words text-xs text-danger">
                  {{ r.server.failure_reason }}
                </span>
              </template>
              <Badge v-else tone="ok">Listed</Badge>
            </td>
            <td class="whitespace-nowrap text-xs text-ink-muted"><RelativeTime :value="r.since" :now="now" /></td>
            <td class="whitespace-nowrap text-right">
              <RouterLink v-if="r.server?.state === 'running'" :to="`/me/servers/${r.server.id}/console`" class="btn-ghost btn-sm">
                Console<span class="sr-only"> of {{ plainName(r) }}</span>
              </RouterLink>
              <button v-if="canStop(r)" type="button" class="btn-ghost btn-sm" :disabled="busy" @click="stopRow(r)">
                Stop<span class="sr-only">{{ ` ${plainName(r)}` }}</span>
              </button>
              <RouterLink
                v-if="r.server && !r.server.live && canStart"
                :to="{ path: '/me/servers/new', query: { from: String(r.server.id) } }"
                class="btn-ghost btn-sm"
              >
                {{ r.server.state === 'failed' ? 'Try again' : 'Start again' }}<span class="sr-only">: {{ plainName(r) }}</span>
              </RouterLink>
            </td>
          </tr>
        </tbody>
      </table>
    </div>
  </div>
</template>

<script setup lang="ts">
// /me/servers: everything the logged-in account runs in one table, its hosted
// servers (GET /api/me/servers) and the games it hosts itself (GET
// /api/me/hosts; a hosted server's own listing joins its row), filtered by the
// URL's state; its allowance in the header with the start while either pool
// has room, else why not. Stops a server, opens its console, starts an ended
// one again. The hosted servers poll on hostedCadence, the listings every
// LIST_POLL_MS.
import { computed, nextTick, onUnmounted, ref } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import GameText from '../../components/GameText.vue';
import HistoryToggle from '../../components/HistoryToggle.vue';
import HostingLockedCallout from '../../components/HostingLockedCallout.vue';
import RelativeTime from '../../components/RelativeTime.vue';
import ServerLink from '../../components/links/ServerLink.vue';
import Badge from '../../components/ui/Badge.vue';
import LiveNotices from '../../components/ui/LiveNotices.vue';
import Notice from '../../components/ui/Notice.vue';
import PageHeader from '../../components/ui/PageHeader.vue';
import TableState from '../../components/ui/TableState.vue';
import { fetchMyServers, stopHostedServer } from '../../api/hostedServer';
import { fetchListingGames, listMyServers, stopHosting } from '../../api/myServers';
import { confirmAction } from '../../composables/useConfirm';
import { hostedCadence, usePolling } from '../../composables/usePolling';
import { useSession } from '../../composables/useSession';
import type { HostedServer, HostedServersResponse } from '../../types/hostedServer';
import type { MyServer } from '../../types/myServers';
import { MY_SERVERS_TABS } from '../../utils/accountNav';
import { serverName } from '../../utils/games';
import { stripGameText } from '../../utils/gameText';
import { replacedServers } from '../../utils/history';
import { ownerStateLabel, phaseSlow, phaseText, stoppedBecauseText } from '../../utils/hostedState';
import {
  allowanceParts,
  blockLine,
  blockLink,
  hostingView,
  listingsOutrunHosted,
  myServerRows,
  sourceTitle,
  startBlock,
  stateOf,
  type MyServerRow,
} from '../../utils/myServersView';
import { serverStateTone } from '../../utils/tone';

// The listings refresh themselves on this period, and on demand.
const LIST_POLL_MS = 10_000;
const OVER_TEXT = 'More are live than you may run: the newest are being stopped.';
const STATE_CHIPS = [
  { value: 'live', label: 'Live' },
  { value: 'ended', label: 'Ended' },
  { value: 'all', label: 'All' },
] as const;

const { refresh: refreshSession } = useSession();
const route = useRoute();
const router = useRouter();

// Each null until its first read answered.
const data = ref<HostedServersResponse | null>(null);
const hosts = ref<MyServer[] | null>(null);
// The game each listing lists under, by its RID.
const games = ref<ReadonlyMap<number, string>>(new Map());
const hostedError = ref('');
const listError = ref('');
const actionError = ref('');
const busy = ref(false);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const pageStatus = ref('');
const pollTrouble = ref(false);
const showHistory = ref(false);
const tableBox = ref<HTMLElement | null>(null);

const state = computed(() => stateOf(route.query));
const view = computed(() => hostingView(data.value, hostedError.value !== ''));
const block = computed(() => (data.value ? startBlock(data.value) : null));
const canStart = computed(() => data.value !== null && block.value === null);
const link = computed(() => (data.value && block.value ? blockLink(block.value, data.value.entitlement) : null));
const allowance = computed(() => (data.value ? allowanceParts(data.value.entitlement) : []));
const allowanceIds = computed(() => allowance.value.map((p) => `servers-allowance-${p.key}`).join(' ') || undefined);
const source = computed(() => (data.value ? sourceTitle(data.value.entitlement) : undefined));
const live = computed(() => (data.value?.servers ?? []).filter((s) => s.live));
const stopping = computed(() => live.value.some((s) => s.state === 'stopping'));
// The fleet's launches are held while one of them waits for a machine.
const launchesHeld = computed(() => live.value.some((s) => s.phase?.name === 'launches_held'));
// An ended server started again since (one of the same name after it) is
// history, shown on request (utils/history.ts).
const replacedIds = computed(() => replacedServers(data.value?.servers ?? []));
const rows = computed(() =>
  myServerRows(data.value?.servers ?? [], hosts.value, state.value, games.value, showHistory.value ? new Set() : replacedIds.value),
);
const emptyText = computed(() => {
  const any = (data.value?.servers.length ?? 0) > 0 || (hosts.value ?? []).some((h) => h.hosted_server_id == null);
  if (!any) return 'No servers yet.';
  return state.value === 'ended' ? 'None ended.' : 'None live.';
});

// Each message once (both reads can fail alike, with no connection).
const pageError = computed(() => {
  const messages = [actionError.value, hostedError.value, listError.value];
  return messages.filter((m, i) => m !== '' && messages.indexOf(m) === i).join(' ');
});

function phaseLine(s: HostedServer): string {
  return s.live && s.phase ? phaseText(s.phase) : '';
}

function plainName(r: MyServerRow): string {
  return stripGameText(serverName(r.name));
}

function canStop(r: MyServerRow): boolean {
  return r.server ? r.server.live && r.server.state !== 'stopping' : true;
}

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
// The games the account hosts itself.

// The listings whose game the page has asked the lobby for: a new one asks
// again, once.
const askedRids = new Set<number>();

async function readGames(listed: readonly MyServer[]) {
  const fresh = listed.filter((h) => h.hosted_server_id == null && !askedRids.has(h.rid));
  if (fresh.length === 0) return;
  for (const h of fresh) askedRids.add(h.rid);
  const found = await fetchListingGames();
  if (found) games.value = found;
}

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
      void readGames(result.value);
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

const loading = computed(() => hostedPoll.loading.value || listPoll.loading.value);

function refresh() {
  pageStatus.value = '';
  actionError.value = '';
  void hostedPoll.refresh();
  void listPoll.refresh();
}

// After a stop: a list read right now could still show the game, so look
// again shortly.
let listSoonTimer: ReturnType<typeof setTimeout> | undefined;
function listSoon() {
  if (listSoonTimer) clearTimeout(listSoonTimer);
  listSoonTimer = setTimeout(() => void listPoll.readAgain(), 1500);
}
onUnmounted(() => {
  if (listSoonTimer) clearTimeout(listSoonTimer);
});

// ---------------------------------------------------------------------------
// Stop.

async function stopRow(r: MyServerRow) {
  if (busy.value) return;
  const name = plainName(r);
  const server = r.server;
  const question = server ? `Stop ${name}? Its match ends for everyone.` : `Stop hosting ${name}? Its match ends for everyone.`;
  if (!(await confirmAction(question))) return;
  busy.value = true;
  pageStatus.value = '';
  actionError.value = '';
  if (server) {
    const result = await stopHostedServer(server.id);
    busy.value = false;
    if (!result.ok) {
      actionError.value = result.error.message;
      if (result.error.code === 'unauthorized') await onSessionEnded();
      else if (result.error.code === 'not_found' || result.error.code === 'not_live') void hostedPoll.readAgain();
      return;
    }
    pageStatus.value = result.value.state === 'stopped' ? `${name} has stopped.` : `${name} is stopping.`;
    void hostedPoll.readAgain();
    return;
  }
  const rid = r.listing!.rid;
  const result = await stopHosting(rid);
  busy.value = false;
  if (!result.ok) {
    actionError.value = result.error.message;
    if (result.error.code === 'unauthorized') await onSessionEnded();
    else if (result.error.code === 'unknown_rid' || result.error.code === 'not_hosting') void listPoll.readAgain();
    return;
  }
  // The row goes at once, and the keyboard to the table, the row it was on
  // having gone.
  pageStatus.value = `Stopped hosting ${name}.`;
  if (hosts.value !== null) hosts.value = hosts.value.filter((h) => h.rid !== rid);
  listSoon();
  await nextTick();
  tableBox.value?.focus();
}
</script>
