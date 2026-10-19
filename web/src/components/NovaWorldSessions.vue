<template>
  <section class="space-y-6" :aria-labelledby="`${idBase}-heading`">
    <div class="space-y-1">
      <div class="flex flex-col gap-1 sm:flex-row sm:items-baseline sm:justify-between">
        <h2 :id="`${idBase}-heading`" class="text-xl font-semibold text-ink">Signed in now</h2>
        <p v-if="loadedAt" class="text-xs text-ink-muted">Read <RelativeTime :value="loadedAt" :now="now" />, again every 15 seconds</p>
      </div>
      <p class="sessions-summary text-sm text-ink-muted">
        Game clients and servers signed in to NovaWorld.
        <span v-if="data">{{ summary }}</span>
      </p>
    </div>

    <!-- Live regions, always in the page so a screen reader hears what lands in
         them, each its own box while it holds something. -->
    <div role="status" :class="message ? 'rounded-panel border border-online/40 bg-online/10 p-4 text-sm text-online' : 'sr-only'">{{ message }}</div>
    <div role="alert" :class="error ? 'rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger' : 'sr-only'">{{ error }}</div>

    <!-- The clients, then the servers (the sessions that host), each its own table. -->
    <div v-for="g in groups" :key="g.key" class="space-y-3" :class="`sessions-${g.key}`">
      <h3 :id="`${idBase}-${g.key}`" class="text-lg font-semibold text-ink">{{ g.heading }}</h3>
      <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
        <table class="min-w-full divide-y divide-border">
          <caption class="sr-only">{{ g.caption }}</caption>
          <thead>
            <tr class="bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted">
              <th
                v-for="col in g.columns"
                :key="col.label"
                scope="col"
                class="whitespace-nowrap px-4 py-3"
                :class="{ 'text-right': col.label === 'Actions' }"
                :aria-sort="ariaSort(col.key)"
              >
                <button
                  v-if="col.key"
                  type="button"
                  class="inline-flex items-center gap-1 uppercase tracking-wider hover:text-ink"
                  @click="sortBy(col.key)"
                >
                  {{ col.label }}<span aria-hidden="true">{{ sortMark(col.key) }}</span>
                </button>
                <template v-else>{{ col.label }}</template>
              </th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border text-sm text-ink">
            <tr v-if="data === null">
              <td :colspan="g.columns.length" class="px-4 py-6 text-center text-ink-muted">{{ poll.loading.value ? 'Loading…' : 'Not loaded.' }}</td>
            </tr>
            <tr v-else-if="g.rows.length === 0">
              <td :colspan="g.columns.length" class="px-4 py-6 text-center text-ink-muted">{{ g.empty }}</td>
            </tr>
            <tr v-for="c in g.rows" :key="c.session_id" class="session-row align-top hover:bg-hover">
              <th scope="row" class="whitespace-nowrap px-4 py-3 text-left font-normal">
                <span class="block font-mono text-xs">{{ c.id }}</span>
                <span class="block text-xs text-ink-muted">{{ sessionStateLabel(c.state) }}</span>
              </th>
              <td class="session-account whitespace-nowrap px-4 py-3">
                <template v-if="c.account">
                  <RouterLink :to="`/admin/users/${c.account.id}`" class="text-accent hover:text-ink-bright">{{ c.account.username }}</RouterLink>
                  <span class="block text-xs text-ink-muted">{{ c.account.via === 'host_key' ? 'By its host key' : 'By its game login' }}</span>
                </template>
                <span v-else-if="c.state === 'handshaking'" class="text-ink-muted">Not signed in yet</span>
                <span v-else class="text-ink-muted" title="No game login or host key it sent names an account">Not known</span>
              </td>
              <td class="session-game whitespace-nowrap px-4 py-3">
                <span v-if="sessionGame(c)">{{ sessionGame(c) }}</span>
                <span v-else class="text-ink-muted" title="Its game tag names no game this NovaWorld runs">Not known</span>
                <span v-if="c.identity" class="block font-mono text-xs text-ink-muted" title="The game tag it signed in with">{{ c.identity }}</span>
              </td>
              <td class="session-lobby whitespace-nowrap px-4 py-3">
                <template v-if="c.lobby_id">
                  <RouterLink
                    :to="lobbyUsageLink(c.lobby_task_id)"
                    class="text-accent hover:text-ink-bright"
                    :title="c.lobby_task_id ? `The usage of task ${c.lobby_task_id}` : 'The usage of the lobby program'"
                  >
                    Lobby {{ c.lobby_id }}
                  </RouterLink>
                  <span v-if="c.lobby_task_id" class="block font-mono text-xs text-ink-muted">{{ shortTaskId(c.lobby_task_id) }}</span>
                </template>
                <span v-else class="text-ink-muted">None</span>
              </td>
              <template v-if="g.key === 'clients'">
                <td class="session-playing px-4 py-3">
                  <template v-if="c.playing">
                    <RouterLink
                      v-if="c.playing.listed && c.playing.hosted_server_id !== null"
                      :to="`/admin/fleet/${c.playing.hosted_server_id}`"
                      class="break-words text-accent hover:text-ink-bright"
                    >
                      {{ c.playing.server_name || 'Unnamed server' }}
                    </RouterLink>
                    <RouterLink
                      v-else-if="c.playing.listed && c.playing.rid !== null"
                      :to="`/lobby/${c.playing.rid}`"
                      class="break-words text-accent hover:text-ink-bright"
                    >
                      {{ c.playing.server_name || 'Unnamed server' }}
                    </RouterLink>
                    <span v-else class="font-mono text-xs" title="No server NovaWorld lists is at this address">{{ c.playing.addr }}</span>
                    <span class="block text-xs text-ink-muted">
                      {{ c.playing.listed ? c.playing.addr : 'Not listed' }}<template v-if="sessionTime(c.playing.since_ms, now) !== null"
                        >, for {{ formatDuration(now - c.playing.since_ms) }}</template
                      >
                    </span>
                  </template>
                  <span v-else class="text-ink-muted">Not playing</span>
                </td>
              </template>
              <template v-else>
                <td class="session-server px-4 py-3">
                  <template v-if="c.host">
                    <RouterLink
                      v-if="c.host.hosted_server_id !== null"
                      :to="`/admin/fleet/${c.host.hosted_server_id}`"
                      class="break-words text-accent hover:text-ink-bright"
                    >
                      {{ c.host.server_name || 'Unnamed server' }}
                    </RouterLink>
                    <RouterLink v-else-if="c.host.listed" :to="`/lobby/${c.host.rid}`" class="break-words text-accent hover:text-ink-bright">
                      {{ c.host.server_name || 'Unnamed server' }}
                    </RouterLink>
                    <span v-else class="break-words">{{ c.host.server_name || 'Unnamed server' }}</span>
                    <span class="block text-xs text-ink-muted">
                      <template v-if="c.host.listed">{{ c.host.mission_name || 'No map yet' }}</template>
                      <template v-else>Not listed: waiting for a host key that names an account</template>
                    </span>
                    <span class="block text-xs text-ink-muted">
                      {{ serverKindLabel(c.host.server_kind) }}<template v-if="serverRoleLabel(c.host.dedicated)">, {{ serverRoleLabel(c.host.dedicated) }}</template>
                    </span>
                  </template>
                  <span v-else class="text-ink-muted">Not known</span>
                </td>
                <td class="session-players whitespace-nowrap px-4 py-3">
                  <template v-if="c.host && c.host.listed">
                    {{ c.host.players }} of {{ c.host.max_players }}
                    <span v-if="clientsOn(c.host.rid) > 0" class="block text-xs text-ink-muted">{{ countLabel(clientsOn(c.host.rid), 'client') }} above</span>
                  </template>
                  <span v-else class="text-ink-muted">None listed</span>
                </td>
              </template>
              <td class="session-addr px-4 py-3 font-mono text-xs">{{ c.addr }}</td>
              <td class="session-connected whitespace-nowrap px-4 py-3 text-xs">
                <template v-if="sessionTime(c.created_ms, now) !== null">
                  <time :datetime="isoTime(c.created_ms)">{{ formatUtc(c.created_ms) }}</time>
                  <span class="block text-ink-muted">for {{ formatDuration(now - c.created_ms) }}</span>
                </template>
                <span v-else class="text-ink-muted">Unknown</span>
              </td>
              <td class="session-heard whitespace-nowrap px-4 py-3 text-xs text-ink-muted">
                <RelativeTime v-if="sessionTime(c.last_seen_ms, now) !== null" :value="c.last_seen_ms" :now="now" />
                <template v-else>Unknown</template>
              </td>
              <td class="whitespace-nowrap px-4 py-3 text-right">
                <button
                  v-if="c.host"
                  type="button"
                  :disabled="busy"
                  class="text-danger/80 hover:text-danger disabled:opacity-40"
                  @click="stopServer(c)"
                >
                  Stop server<span class="sr-only"> {{ c.host.server_name }}</span>
                </button>
                <button
                  v-else
                  type="button"
                  :disabled="busy"
                  class="text-danger/80 hover:text-danger disabled:opacity-40"
                  @click="endSession(c)"
                >
                  End<span class="sr-only"> session {{ c.id }}</span>
                </button>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </div>
  </section>
</template>

<script setup lang="ts">
// The NovaWorld sessions the service holds now (GET /api/admin/connections), on
// the NovaWorld page, in two tables: the clients, each with the server it plays
// on (the listing, linked to its page, or to /admin/fleet/<id> for a hosted
// server of the fleet's; its address alone for a server no lobby lists), and
// the servers (the sessions that host: a listen host plays on its own server
// too), each with its listing, what it is (a hosted server, a player's own or
// anonymous; Serve Only or listen) and its players. Every row names its account
// (the owner its host key names, or the account it signed in as), its game, the
// lobby instance holding it (linked to that lobby task's usage), its address,
// when it connected (the UTC time and for how long) and when it was last heard
// from. Both sort by the same header (session, account or either time); polled
// every 15 s. End asks the service to end a client's session (the client is told
// and signs in again); a server stops instead, through the listed server's stop.
// refresh() reads it again (the page's Refresh).
import { computed, ref } from 'vue';
import { RouterLink } from 'vue-router';
import RelativeTime from './RelativeTime.vue';
import { endConnection, fetchConnections, stopListedServer } from '../api/admin';
import { usePolling } from '../composables/usePolling';
import type { AdminConnection, ConnectionsResponse } from '../types/admin';
import {
  lobbyUsageLink,
  serverKindLabel,
  serverRoleLabel,
  sessionGame,
  sessionStateLabel,
  sessionTime,
  shortTaskId,
  sortSessions,
  type SessionSortKey,
} from '../utils/adminView';
import { formatDuration, formatUtc, isoTime } from '../utils/format';
import { countLabel } from '../utils/infraView';

withDefaults(defineProps<{ idBase?: string }>(), { idBase: 'sessions' });

const POLL_MS = 15_000;

type Column = { label: string; key: SessionSortKey | null };
const leading: Column[] = [
  { label: 'Session', key: 'id' },
  { label: 'Account', key: 'account' },
  { label: 'Game', key: null },
  { label: 'Lobby', key: null },
];
const trailing: Column[] = [
  { label: 'Address', key: null },
  { label: 'Connected', key: 'connected' },
  { label: 'Last heard', key: 'last_heard' },
  { label: 'Actions', key: null },
];
const clientColumns: Column[] = [...leading, { label: 'Playing on', key: null }, ...trailing];
const serverColumns: Column[] = [...leading, { label: 'Server', key: null }, { label: 'Players', key: null }, ...trailing];

const data = ref<ConnectionsResponse | null>(null);
const error = ref('');
const message = ref('');
const busy = ref(false);
const now = ref(Date.now());
const loadedAt = ref<number | null>(null);
// Newest first until a header says otherwise.
const sortKey = ref<SessionSortKey>('connected');
const ascending = ref(false);
// The error banner shows a failed read (not an action's refusal).
let errorFromLoad = false;

const rows = computed(() => sortSessions(data.value?.connections ?? [], sortKey.value, ascending.value, now.value));
const clients = computed(() => rows.value.filter((c) => c.kind === 'client'));
const servers = computed(() => rows.value.filter((c) => c.kind === 'server'));

const groups = computed(() => [
  {
    key: 'clients',
    heading: 'Clients',
    caption: 'Game clients signed in to NovaWorld, as the service holds them now',
    empty: 'No game client is signed in.',
    columns: clientColumns,
    rows: clients.value,
  },
  {
    key: 'servers',
    heading: 'Servers',
    caption: 'Servers signed in to NovaWorld (the sessions that host), as the service holds them now',
    empty: 'No server is signed in.',
    columns: serverColumns,
    rows: servers.value,
  },
]);

// The client sessions playing on the listing `rid`.
function clientsOn(rid: number): number {
  return clients.value.filter((c) => c.playing?.rid === rid && c.playing.listed).length;
}

// "3 signed in: 2 clients and 1 server; a session closes after 4 minutes without a keepalive."
const summary = computed(() => {
  const d = data.value;
  if (!d) return '';
  const signedIn = `${d.count} signed in: ${countLabel(clients.value.length, 'client')} and ${countLabel(servers.value.length, 'server')}`;
  if (!d.heartbeat_timeout_ms) return `${signedIn}.`;
  return `${signedIn}; a session closes after ${formatDuration(d.heartbeat_timeout_ms)} without a keepalive.`;
});

// A header's sort (both tables follow it): a new column starts as its natural
// order (ids and names ascending, times newest first), the same column again
// reverses it.
function sortBy(key: SessionSortKey) {
  if (sortKey.value === key) {
    ascending.value = !ascending.value;
    return;
  }
  sortKey.value = key;
  ascending.value = key === 'id' || key === 'account';
}

function ariaSort(key: SessionSortKey | null): 'ascending' | 'descending' | undefined {
  if (key === null || key !== sortKey.value) return undefined;
  return ascending.value ? 'ascending' : 'descending';
}

function sortMark(key: SessionSortKey): string {
  if (key !== sortKey.value) return '';
  return ascending.value ? ' ▲' : ' ▼';
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

function who(c: AdminConnection): string {
  return c.account ? `${c.account.username}'s` : 'the';
}

async function endSession(c: AdminConnection) {
  if (busy.value) return;
  if (!window.confirm(`End ${who(c)} NovaWorld session ${c.id} (${c.addr})? The game is told the session ended and has to sign in again.`)) return;
  busy.value = true;
  const result = await endConnection(c);
  busy.value = false;
  if (!result.ok) fail(result.error.message);
  else flash(`Session ${c.id} is ending.`);
  await poll.readAgain();
}

async function stopServer(c: AdminConnection) {
  const host = c.host;
  if (busy.value || !host) return;
  const players = host.players ? ` The match ends for the ${host.players} player${host.players === 1 ? '' : 's'} on it.` : '';
  if (!window.confirm(`Stop ${who(c)} server ${host.server_name || host.rid}? It leaves NovaWorld.${players}`)) return;
  busy.value = true;
  const result = await stopListedServer(host.rid);
  busy.value = false;
  if (!result.ok) fail(result.error.message);
  else flash(`${host.server_name || 'The server'} is stopping.`);
  await poll.readAgain();
}

const poll = usePolling({
  async read(signal, background) {
    const result = await fetchConnections(signal);
    if (signal.aborted) return false;
    now.value = Date.now();
    if (result.ok) {
      data.value = result.value;
      loadedAt.value = now.value;
      if (errorFromLoad) {
        error.value = '';
        errorFromLoad = false;
      }
      return true;
    }
    if (!background || data.value === null) {
      fail(result.error.message);
      errorFromLoad = true;
    }
    return false;
  },
  next: () => POLL_MS,
});

defineExpose({ refresh: poll.refresh, loading: poll.loading });
</script>
