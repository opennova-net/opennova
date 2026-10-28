<template>
  <div class="space-y-6">
    <PageHeader title="Servers" :tabs="SERVERS_TABS" refreshable :loaded-at="loadedAt" :now="now" :loading="poll.loading.value" @refresh="poll.refresh">
      <template #actions>
        <button
          type="button"
          class="btn-secondary"
          :disabled="busy || outdatedCount === 0 || !!filter.owner"
          :title="filter.owner ? 'Clear the owner filter to update' : undefined"
          @click="updateOutdated"
        >
          Update outdated…
        </button>
        <details ref="stopAllBox" class="stop-all relative">
          <summary class="btn-danger cursor-pointer list-none">Stop all…</summary>
          <form class="card absolute right-0 z-20 mt-2 w-72 space-y-3 p-3" @submit.prevent="confirmStopAll">
            <p class="text-sm text-danger">Stop all {{ fleet?.live ?? 0 }} live hosted servers? Every match on them ends.</p>
            <div>
              <label for="stop-all-confirm" class="field-label">Type <span class="font-mono">{{ STOP_ALL_PHRASE }}</span></label>
              <input id="stop-all-confirm" v-model="typedPhrase" type="text" autocomplete="off" autocapitalize="characters" spellcheck="false" class="field font-mono" />
            </div>
            <button type="submit" class="btn-danger w-full" :disabled="busy || typedPhrase.trim() !== STOP_ALL_PHRASE">
              {{ busy ? 'Stopping…' : 'Stop all now' }}
            </button>
          </form>
        </details>
      </template>
    </PageHeader>

    <LiveNotices :message="message" :error="error" :poll-trouble="pollTrouble" />
    <Notice v-if="listedTrouble" tone="warn" class="listed-trouble">Could not read the player-hosted servers ({{ listedTrouble }}).</Notice>

    <dl class="servers-tiles grid grid-cols-1 gap-3 min-[420px]:grid-cols-2 lg:grid-cols-4">
      <StatTile
        label="Live servers"
        :value="fleet ? formatCount(fleet.live + playerHosted) : '…'"
        :detail="fleet ? `${formatCount(fleet.live)} hosted, ${formatCount(playerHosted)} player-hosted` : undefined"
      />
      <StatTile label="Players" :value="fleet ? formatCount(players) : '…'" />
      <StatTile label="Machines" to="/admin/servers/machines" :value="fleet ? formatCount(activeMachines.length) : '…'" :detail="fleet ? machineBreakdown : undefined" />
      <StatTile
        label="Hosting"
        :value="fleet ? (fleet.enabled ? 'On' : 'Off') : '…'"
        :detail="fleet ? `${fleet.shared.live}/${fleet.shared.room} shared, ${fleet.own.live}/${fleet.own.room} dedicated` : undefined"
        :tone="fleet && !fleet.enabled ? 'warn' : undefined"
      />
    </dl>

    <div class="servers-filters flex flex-wrap items-center gap-x-4 gap-y-2">
      <div role="group" aria-label="State" class="flex gap-1">
        <RouterLink
          v-for="c in STATE_CHIPS"
          :key="c.value"
          :to="withQuery({ state: c.value === 'live' ? undefined : c.value })"
          :class="chip(filter.state === c.value)"
          :aria-current="filter.state === c.value ? 'true' : undefined"
        >
          {{ c.label }}
        </RouterLink>
      </div>
      <div role="group" aria-label="Kind" class="flex gap-1">
        <RouterLink
          v-for="c in KIND_CHIPS"
          :key="c.value"
          :to="withQuery({ kind: c.value === 'all' ? undefined : c.value })"
          :class="chip(filter.kind === c.value)"
          :aria-current="filter.kind === c.value ? 'true' : undefined"
        >
          {{ c.label }}
        </RouterLink>
      </div>
      <label for="servers-product" class="sr-only">Game</label>
      <select id="servers-product" class="field w-auto" :value="filter.product ?? ''" @change="onProduct">
        <option value="">All games</option>
        <option v-for="p in productOptions" :key="p.id" :value="p.id">{{ p.name }}</option>
      </select>
      <form role="search" @submit.prevent="applyOwner">
        <label for="servers-owner" class="sr-only">Owner</label>
        <input id="servers-owner" v-model="ownerInput" type="search" autocomplete="off" placeholder="Owner" class="field w-auto" />
      </form>
      <HistoryToggle v-if="filter.state === 'all'" v-model="showHistory" :count="replacedIds.size" />
    </div>

    <div class="table-wrap">
      <table class="data-table">
        <caption class="sr-only">Servers</caption>
        <thead>
          <tr>
            <th scope="col">Server</th>
            <th scope="col">Map</th>
            <th scope="col">Players</th>
            <th scope="col">Owner</th>
            <th scope="col">Runs on</th>
            <th scope="col">State</th>
            <th scope="col">Release</th>
            <th scope="col">CPU / Mem</th>
            <th scope="col">Since</th>
            <th scope="col"><span class="sr-only">Actions</span></th>
          </tr>
        </thead>
        <tbody>
          <TableState v-if="rows.length === 0" :colspan="10" :rows="fleet === null ? null : rows" :loading="poll.loading.value" empty="No servers match." />
          <tr v-for="r in rows" :key="r.key" class="server-row" :class="r.server ? 'hosted-row' : 'listed-row'">
            <th scope="row" class="min-w-[12rem]">
              <ServerLink :name="r.name" :id="r.server?.id ?? null" :rid="r.host?.rid ?? null" scope="admin" />
              <span class="server-game block text-xs font-normal text-ink-muted">
                {{ r.game }}<template v-if="r.gameType"> · <GameText :text="r.gameType" /></template>
              </span>
            </th>
            <td class="server-map"><GameText v-if="r.map" :text="r.map" /><span v-else class="text-ink-muted">—</span></td>
            <td class="whitespace-nowrap">{{ r.players || '—' }}</td>
            <td class="whitespace-nowrap">
              <template v-if="r.server">
                <span v-if="r.server.owner" :title="ownerHostingLabel(r.server.owner)"><PlayerLink :id="r.server.owner.id" :name="r.server.owner.username" /></span>
                <PlayerLink v-else :name="''" />
              </template>
              <PlayerLink v-else-if="r.host?.owner" :id="r.host.owner.id" :name="r.host.owner.username" />
              <span v-else class="text-ink-muted">Anonymous</span>
            </td>
            <td class="server-runs-on whitespace-nowrap">
              <template v-if="r.server">
                <template v-if="r.server.machine_ref">
                  <MachineLink :id="r.server.machine_ref" />
                  <span class="text-xs text-ink-muted"> slot {{ r.server.slot ?? '—' }}</span>
                </template>
                <span v-else class="text-ink-muted">Not placed</span>
              </template>
              <template v-else-if="r.host">
                Player-hosted
                <span class="block font-mono text-xs text-ink-muted">{{ r.host.host_ip }}:{{ r.host.host_port }}</span>
              </template>
            </td>
            <td class="whitespace-nowrap">
              <template v-if="r.server">
                <Badge :tone="serverStateTone(r.server.state)">{{ stateLabel(r.server.state) }}</Badge>
                <span v-if="r.server.restarts > 0" class="block text-xs text-warn">{{ r.server.restarts }} restart{{ r.server.restarts === 1 ? '' : 's' }}</span>
                <span v-if="r.server.state === 'failed' && r.server.failure_reason" class="block max-w-xs whitespace-normal text-xs text-danger">{{ r.server.failure_reason }}</span>
              </template>
              <Badge v-else tone="ok">Listed</Badge>
            </td>
            <td class="server-release whitespace-nowrap text-xs">
              <template v-if="r.server && r.server.base_tree_id !== null">
                <ProductLink v-if="r.server.product" :product-ref="r.server.product.key || r.server.product.id" :name="r.server.data.base?.version || 'No version'" />
                <span v-else>{{ r.server.data.base?.version || 'No version' }}</span>
                <span v-if="r.server.expansion_tree_id !== null" class="block text-ink-muted">
                  {{ r.server.expansion?.display_name || 'Expansion' }} {{ r.server.data.expansion?.version || '' }}
                </span>
                <Badge v-if="r.server.update_requested" tone="info">Update requested</Badge>
                <Badge v-else-if="r.server.outdated" tone="warn">Outdated</Badge>
              </template>
              <span v-else class="text-ink-muted">—</span>
            </td>
            <td class="server-usage whitespace-nowrap text-xs">{{ usageText(r.server) }}</td>
            <td class="whitespace-nowrap text-xs text-ink-muted"><RelativeTime v-if="r.since" :value="r.since" :now="now" /></td>
            <td class="whitespace-nowrap text-right">
              <RouterLink v-if="r.server?.state === 'running'" :to="`/admin/servers/${r.server.id}/console`" class="btn-ghost btn-sm">
                Console<span class="sr-only"> of {{ plainName(r) }}</span>
              </RouterLink>
              <button v-if="canStop(r)" type="button" class="btn-ghost btn-sm" :disabled="busy" @click="stopRow(r)">
                Stop<span class="sr-only">{{ ' ' + (plainName(r)) }}</span>
              </button>
            </td>
          </tr>
        </tbody>
      </table>
    </div>
  </div>
</template>

<script setup lang="ts">
// /admin/servers: every server on our NovaWorld in one table, the fleet's
// hosted servers (GET /api/admin/hosted) and the ones players host (GET
// /api/admin/hosts), each with its game, map, players, owner, where it runs,
// state, release and what it uses (GET /api/admin/hosted/usage); filtered by
// the URL's state, kind, product and owner. Updates the outdated servers and
// stops one or all. Polls, faster while a server changes state.
import { computed, ref, watch } from 'vue';
import { RouterLink, useRoute, useRouter, type LocationQueryRaw } from 'vue-router';
import GameText from '../../../components/GameText.vue';
import HistoryToggle from '../../../components/HistoryToggle.vue';
import RelativeTime from '../../../components/RelativeTime.vue';
import StatTile from '../../../components/charts/StatTile.vue';
import MachineLink from '../../../components/links/MachineLink.vue';
import PlayerLink from '../../../components/links/PlayerLink.vue';
import ProductLink from '../../../components/links/ProductLink.vue';
import ServerLink from '../../../components/links/ServerLink.vue';
import Badge from '../../../components/ui/Badge.vue';
import LiveNotices from '../../../components/ui/LiveNotices.vue';
import Notice from '../../../components/ui/Notice.vue';
import PageHeader from '../../../components/ui/PageHeader.vue';
import TableState from '../../../components/ui/TableState.vue';
import { fetchHostedFleet, fetchListedHosts, stopAllFleetServers, stopFleetServer, stopListedServer } from '../../../api/admin';
import { fetchFleetUsage } from '../../../api/hostedStats';
import { fetchProducts, updateAllServers } from '../../../api/products';
import { confirmAction } from '../../../composables/useConfirm';
import { usePolling } from '../../../composables/usePolling';
import type { ListedHost } from '../../../types/admin';
import type { FleetResponse, FleetServer } from '../../../types/hostedServer';
import type { FleetUsage } from '../../../types/hostedStats';
import type { ProductsResponse } from '../../../types/products';
import { SERVERS_TABS } from '../../../utils/adminNav';
import { ownerHostingLabel } from '../../../utils/adminView';
import { formatCount, formatMemory, formatPercent } from '../../../utils/format';
import { serverName } from '../../../utils/games';
import { stripGameText } from '../../../utils/gameText';
import { replacedServers } from '../../../utils/history';
import { isTransitioning, stateLabel } from '../../../utils/hostedState';
import { filterOf, runsProduct, serverRows, type ServerRow, type ServersFilter } from '../../../utils/serversView';
import { serverStateTone } from '../../../utils/tone';

const STOP_ALL_PHRASE = 'STOP ALL';
const TRANSITION_POLL_MS = 5_000;
const IDLE_POLL_MS = 30_000;
const STATE_CHIPS = [
  { value: 'live', label: 'Live' },
  { value: 'ended', label: 'Ended' },
  { value: 'all', label: 'All' },
] as const;
const KIND_CHIPS = [
  { value: 'all', label: 'All' },
  { value: 'hosted', label: 'Hosted' },
  { value: 'player', label: 'Player-hosted' },
] as const;

const route = useRoute();
const router = useRouter();
const filter = computed<ServersFilter>(() => filterOf(route.query));

const fleet = ref<FleetResponse | null>(null);
const hosts = ref<ListedHost[]>([]);
const usage = ref<FleetUsage | null>(null);
const products = ref<ProductsResponse | null>(null);
const busy = ref(false);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const message = ref('');
const error = ref('');
const pollTrouble = ref(false);
const listedTrouble = ref('');
const typedPhrase = ref('');
const stopAllBox = ref<HTMLDetailsElement | null>(null);
const ownerInput = ref(filter.value.owner);
const showHistory = ref(false);
// The error shown is a failed read's (not an action's refusal).
let errorFromLoad = false;

// A filter chip: the one in force stands out.
function chip(active: boolean): string {
  return active ? 'btn-secondary btn-sm' : 'btn-ghost btn-sm';
}

function withQuery(change: Record<string, string | undefined>): { path: string; query: LocationQueryRaw } {
  return { path: route.path, query: { ...route.query, ...change } };
}

function onProduct(event: Event) {
  const value = (event.target as HTMLSelectElement).value;
  void router.replace(withQuery({ product: value || undefined }));
}

function applyOwner() {
  const owner = ownerInput.value.trim();
  if (owner === filter.value.owner) return;
  void router.replace(withQuery({ owner: owner || undefined }));
}

const replacedIds = computed(() =>
  filter.value.state === 'all' ? replacedServers(fleet.value?.servers ?? [], (s) => s.owner?.id ?? null) : new Set<number>(),
);
const rows = computed(() =>
  serverRows(fleet.value?.servers ?? [], hosts.value, filter.value, products.value, showHistory.value ? new Set() : replacedIds.value),
);

const playerHosted = computed(() => hosts.value.filter((h) => h.hosted_server_id === null).length);
// Everyone on a listed server; the fleet's own count while the listings are unread.
const players = computed(() =>
  hosts.value.length
    ? hosts.value.reduce((n, h) => n + h.players, 0)
    : (fleet.value?.servers ?? []).reduce((n, s) => n + (s.live ? s.players ?? 0 : 0), 0),
);
const activeMachines = computed(() => (fleet.value?.machines ?? []).filter((m) => m.state !== 'ended' && m.state !== 'failed'));
const machineBreakdown = computed(() => {
  const shared = activeMachines.value.filter((m) => m.kind === 'shared').length;
  return `${shared} shared, ${activeMachines.value.length - shared} dedicated`;
});
const outdatedCount = computed(
  () =>
    (fleet.value?.servers ?? []).filter(
      (s) => s.live && s.outdated && !s.update_requested && (filter.value.product === null || runsProduct(s, filter.value.product)),
    ).length,
);

// The games the filter offers: every game and expansion, else the ones the
// fleet's rows name while the products are unread.
const productOptions = computed(() => {
  const list = products.value
    ? products.value.products.filter((p) => p.kind !== 'app').map((p) => ({ id: p.id, name: p.display_name }))
    : [...new Map((fleet.value?.servers ?? []).flatMap((s) => (s.product ? [[s.product.id, { id: s.product.id, name: s.product.display_name }] as const] : []))).values()];
  return list.sort((a, b) => a.name.localeCompare(b.name));
});

const usageById = computed(() => new Map((usage.value?.servers ?? []).map((u) => [u.id, u])));
function usageText(s: FleetServer | null): string {
  const u = s ? usageById.value.get(s.id) : undefined;
  if (!u || (u.cpu_percent === null && u.memory_bytes === null)) return '—';
  return [u.cpu_percent !== null ? formatPercent(u.cpu_percent) : '—', u.memory_bytes !== null ? formatMemory(u.memory_bytes) : '—'].join(' · ');
}

function plainName(r: ServerRow): string {
  return stripGameText(serverName(r.name));
}

function canStop(r: ServerRow): boolean {
  return r.server ? r.server.live && r.server.state !== 'stopping' : r.host !== null;
}

function flash(text: string) {
  message.value = text;
  error.value = '';
}

function fail(text: string) {
  error.value = text;
  message.value = '';
  errorFromLoad = false;
}

const poll = usePolling({
  async read(signal, background) {
    const f = filter.value;
    const [fl, ls, us, pr] = await Promise.all([
      fetchHostedFleet({ state: f.state, ...(f.owner ? { owner: f.owner } : {}) }, signal),
      fetchListedHosts(signal),
      fetchFleetUsage(signal),
      // The games change rarely: read once.
      products.value ? Promise.resolve(null) : fetchProducts(signal),
    ]);
    if (signal.aborted) return false;
    now.value = Date.now();
    if (ls.ok) {
      hosts.value = ls.value.hosts;
      listedTrouble.value = '';
    } else {
      listedTrouble.value = ls.error.message;
    }
    // What a server uses and the games' names are niceties: a failure says nothing.
    if (us.ok) usage.value = us.value;
    if (pr && pr.ok) products.value = pr.value;
    if (fl.ok) {
      fleet.value = fl.value;
      loadedAt.value = now.value;
      pollTrouble.value = false;
      if (errorFromLoad) {
        error.value = '';
        errorFromLoad = false;
      }
      return true;
    }
    if (background && fleet.value !== null) {
      pollTrouble.value = true;
    } else {
      fail(fl.error.message);
      errorFromLoad = true;
    }
    return false;
  },
  next: () => {
    const moving =
      (fleet.value?.servers.some((s) => isTransitioning(s.state)) ?? false) ||
      (fleet.value?.machines.some((m) => m.state !== 'ready' && m.state !== 'ended' && m.state !== 'failed') ?? false);
    return moving ? TRANSITION_POLL_MS : IDLE_POLL_MS;
  },
});

// A new state or owner is a new read; the rest filter what is read.
watch(
  () => [filter.value.state, filter.value.owner],
  () => {
    ownerInput.value = filter.value.owner;
    void poll.refresh();
  },
);

async function stopRow(r: ServerRow) {
  if (busy.value) return;
  const name = plainName(r);
  if (!(await confirmAction(`Stop ${name}? Its match ends for everyone on it.`))) return;
  busy.value = true;
  const result = r.server ? await stopFleetServer(r.server.id) : await stopListedServer(r.host!.rid);
  busy.value = false;
  if (!result.ok) fail(result.error.message);
  else flash(`${name} is stopping.`);
  await poll.readAgain();
}

// Every live server on an older release restarts on its game's newest (of the
// filter's game alone when it names one).
async function updateOutdated() {
  if (busy.value || filter.value.owner) return;
  const n = outdatedCount.value;
  const product = filter.value.product;
  if (!(await confirmAction(`Update ${n} outdated server${n === 1 ? '' : 's'}? Each restarts and drops its players.`))) return;
  busy.value = true;
  const result = product !== null ? await updateAllServers(product) : await updateAllServers();
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  const asked = result.value;
  flash(asked === 0 ? 'None needed an update.' : `${asked} server${asked === 1 ? '' : 's'} updating.`);
  await poll.readAgain();
}

async function confirmStopAll() {
  if (busy.value || typedPhrase.value.trim() !== STOP_ALL_PHRASE) return;
  busy.value = true;
  const result = await stopAllFleetServers();
  busy.value = false;
  if (!result.ok) {
    fail(result.error.message);
    return;
  }
  const n = result.value;
  flash(n === 0 ? 'None was live.' : `${n} server${n === 1 ? '' : 's'} stopping.`);
  typedPhrase.value = '';
  if (stopAllBox.value) stopAllBox.value.open = false;
  await poll.readAgain();
}
</script>
