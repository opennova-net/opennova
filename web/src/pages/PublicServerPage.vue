<template>
  <section class="page space-y-6 py-10 sm:py-14">
    <PageHeader
      :title="shown ? stripGameText(name) : 'Server'"
      :crumbs="[{ label: 'All servers', to: '/lobby' }]"
      refreshable
      :loaded-at="loadedAt"
      :now="now"
      :loading="poll.loading.value"
      @refresh="poll.refresh"
    >
      <template v-if="shown" #title><GameText :text="name" /></template>
      <template v-if="shown" #meta>
        <span>{{ shown.game }}</span>
        <span>{{ novalogic ? "NovaLogic's NovaWorld" : "OpenNova's NovaWorld" }}</span>
        <span class="server-players font-mono">{{ shown.players }} / {{ shown.maxPlayers }}<span class="sr-only"> players</span></span>
        <Badge v-if="shown.password">Password</Badge>
        <Badge v-else-if="shown.locked">Locked</Badge>
        <Badge v-if="shown.punkBuster">PunkBuster</Badge>
        <Badge v-if="shown.dedicated">Dedicated</Badge>
      </template>
      <template v-if="manageTo" #actions>
        <RouterLink :to="manageTo" class="btn-primary">Manage</RouterLink>
      </template>
    </PageHeader>

    <Notice v-if="gone" class="server-gone">This server is no longer listed.</Notice>
    <Notice v-else-if="error" tone="bad" role="alert">{{ error }}</Notice>
    <p v-else-if="!shown" role="status" class="text-sm text-ink-muted">{{ waitingText }}</p>

    <template v-if="shown && !gone">
      <Notice v-if="stale || unreachable" tone="warn" role="status">
        {{ unreachable ? "NovaLogic's NovaWorld can't be reached; this is the last list we got." : 'The last refresh failed; this is the last we got.' }}
      </Notice>
      <p v-if="message" class="server-message whitespace-pre-line break-words text-sm text-ink-muted"><GameText :text="message" /></p>

      <p id="join-server" class="text-sm text-ink">
        Join from your game's server browser on {{ novalogic ? "NovaLogic's NovaWorld (Original NovaWorld in OpenNova)" : "OpenNova's NovaWorld" }}.
        <RouterLink :to="novalogic ? '/play?game=jointops#online' : '/play#online'" class="link">How to join</RouterLink>
      </p>

      <dl class="card grid gap-x-6 gap-y-2 p-4 text-sm sm:grid-cols-[auto_1fr]">
        <template v-if="shown.mission || shown.gameType">
          <dt class="text-ink-muted">Mission</dt>
          <dd>
            <GameText :text="shown.mission" /><span v-if="shown.mission && shown.gameType" class="text-ink-muted"> (<GameText :text="shown.gameType" />)</span
            ><GameText v-else-if="shown.gameType" :text="shown.gameType" />
          </dd>
        </template>
        <template v-if="shown.timeLeft">
          <dt class="text-ink-muted">Time left</dt>
          <dd>{{ shown.timeLeft }}</dd>
        </template>
        <template v-if="shown.timeOfDay">
          <dt class="text-ink-muted">Time of day</dt>
          <dd>{{ shown.timeOfDay }}</dd>
        </template>
        <template v-if="shown.expansion">
          <dt class="text-ink-muted">Expansion</dt>
          <dd>{{ shown.expansion }}</dd>
        </template>
        <template v-if="shown.country">
          <dt class="text-ink-muted">Country</dt>
          <dd>{{ shown.country }}</dd>
        </template>
        <template v-if="shown.address">
          <dt class="text-ink-muted">Address</dt>
          <dd class="font-mono">{{ shown.address }}</dd>
        </template>
        <template v-if="shown.tracers !== null">
          <dt class="text-ink-muted">Tracers</dt>
          <dd>{{ shown.tracers ? 'On' : 'Off' }}</dd>
        </template>
        <template v-if="shown.build">
          <dt class="text-ink-muted">Server build</dt>
          <dd class="server-build"><BuildText :build="shown.build" /></dd>
        </template>
      </dl>

      <section class="space-y-3" aria-labelledby="server-players">
        <h2 id="server-players" class="section-heading">Players</h2>
        <div v-if="sortedScores.length > 0" class="table-wrap">
          <table class="data-table">
            <thead>
              <tr>
                <th scope="col">Name</th>
                <th scope="col" class="text-right">Team</th>
                <th scope="col" class="text-right">Kills</th>
                <th scope="col" class="text-right">Deaths</th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="(p, i) in sortedScores" :key="`${i}-${p.name}`">
                <th scope="row" class="break-all">{{ p.name }}</th>
                <td class="text-right font-mono text-xs">{{ p.team }}</td>
                <td class="text-right font-mono text-xs">{{ p.kills }}</td>
                <td class="text-right font-mono text-xs">{{ p.deaths }}</td>
              </tr>
            </tbody>
          </table>
        </div>
        <ul v-else-if="shown.roster.length > 0" class="card divide-y divide-border">
          <li v-for="(player, i) in shown.roster" :key="`${i}-${player}`" class="break-all px-4 py-2 text-sm">{{ player }}</li>
        </ul>
        <p v-else class="text-sm text-ink-muted">No one is playing.</p>
      </section>
    </template>
  </section>
</template>

<script setup lang="ts">
// A listed server's public page, from either source: one our NovaWorld lists
// (/lobby/<id>, GET /api/lobbies/<id>, with its scores and its ONE build on a
// server NovaWorld hosts) or one on NovaLogic's (/lobby/novalogic/<id>, its row
// of the service's copy of their list). Read every 15 s. A signed-in visitor
// whose own listing it is (GET /api/me/hosts) gets a Manage link to it.
import { computed, ref, watch } from 'vue';
import { RouterLink, useRoute, type RouteLocationRaw } from 'vue-router';
import BuildText from '../components/BuildText.vue';
import GameText from '../components/GameText.vue';
import Badge from '../components/ui/Badge.vue';
import Notice from '../components/ui/Notice.vue';
import PageHeader from '../components/ui/PageHeader.vue';
import { fetchLobbyServer, fetchNovaLogicLobbies, LobbyGone } from '../api/lobbies';
import { listMyServers } from '../api/myServers';
import { usePolling } from '../composables/usePolling';
import { useSession } from '../composables/useSession';
import type { LobbyBuild, LobbyScore, LobbyServer, NovaLogicServer } from '../types/lobby';
import { serverName, titleName } from '../utils/games';
import { stripGameText } from '../utils/gameText';

const POLL_MS = 15_000;
// The host screen's message until its host writes one: the menu's ("!Put your message here")
// or game.cfg's default ("Put your message here."), which a host file or a dedicated server
// leaves in.
const DEFAULT_MESSAGES = ['!Put your message here', 'Put your message here.'];

// One server as the page shows it, from either source.
interface Shown {
  serverName: string;
  message: string;
  game: string;
  players: number;
  maxPlayers: number;
  mission: string;
  gameType: string;
  timeLeft: string;
  timeOfDay: string;
  expansion: string;
  country: string;
  address: string;
  password: boolean;
  locked: boolean;
  punkBuster: boolean;
  dedicated: boolean;
  // NovaLogic's list says; ours does not (null).
  tracers: boolean | null;
  roster: string[];
  scoreboard: LobbyScore[] | null;
  // Its server's ONE build, where ours knows it (a server NovaWorld hosts).
  build: LobbyBuild | null;
}

// A host's time of day; "Unknown" is none.
function timeOfDay(text: string | null | undefined): string {
  const t = text?.trim() ?? '';
  return t === 'Unknown' ? '' : t;
}

function fromOurs(s: LobbyServer): Shown {
  return {
    serverName: s.serverName,
    message: s.message,
    game: s.game.displayName || 'NovaWorld',
    players: s.players,
    maxPlayers: s.maxPlayers || 0,
    mission: s.mission || '',
    gameType: s.gameType || '',
    timeLeft: s.timeLeft !== null ? `${s.timeLeft} min` : '',
    timeOfDay: timeOfDay(s.timeOfDay),
    expansion: s.expansion || '',
    country: '',
    address: s.hostIp ? `${s.hostIp}:${s.hostPort}` : '',
    password: Boolean(s.locked),
    locked: false,
    punkBuster: false,
    dedicated: false,
    tracers: null,
    roster: s.roster,
    scoreboard: s.scoreboard,
    build: s.build ?? null,
  };
}

// NovaLogic's time left: minutes as its host wrote them ("29"), or what else it
// wrote; "NA" is none.
function novalogicTimeLeft(text: string): string {
  const t = text.trim();
  if (t === '' || t.toUpperCase() === 'NA') return '';
  return /^\d+$/.test(t) ? `${t} min` : t;
}

function fromNovaLogic(s: NovaLogicServer): Shown {
  return {
    serverName: s.serverName,
    message: s.message,
    game: titleName('jo'),
    players: s.players,
    maxPlayers: s.maxPlayers || 0,
    mission: s.mission,
    gameType: s.gameType,
    timeLeft: novalogicTimeLeft(s.timeLeft),
    timeOfDay: timeOfDay(s.timeOfDay),
    expansion: s.expansion,
    country: [s.country, s.region].filter(Boolean).join(', '),
    address: '',
    password: s.password,
    locked: s.locked,
    punkBuster: s.punkBuster,
    dedicated: s.dedicated,
    tracers: s.tracers,
    roster: s.roster,
    scoreboard: null,
    build: null,
  };
}

const route = useRoute();
const { user } = useSession();

const novalogic = computed(() => route.path.startsWith('/lobby/novalogic/'));
const shown = ref<Shown | null>(null);
const gone = ref(false);
const error = ref('');
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const stale = ref(false);
// NovaLogic's: the service has no list yet, or its last fetch failed.
const awaitingList = ref(false);
const unreachable = ref(false);
// The signed-in visitor's own listing of this server, when it is one.
const manageTo = ref<RouteLocationRaw | null>(null);

const name = computed(() => serverName(shown.value?.serverName));
const message = computed(() => {
  const text = shown.value?.message?.trim() ?? '';
  return DEFAULT_MESSAGES.includes(text) ? '' : text;
});
// Highest kills first.
const sortedScores = computed(() => [...(shown.value?.scoreboard ?? [])].sort((a, b) => b.kills - a.kills || a.deaths - b.deaths));
const waitingText = computed(() => {
  if (unreachable.value) return "NovaLogic's NovaWorld can't be reached right now.";
  if (awaitingList.value) return "Checking NovaLogic's NovaWorld…";
  return 'Loading…';
});

async function readOurs(id: number, signal: AbortSignal): Promise<void> {
  const result = await fetchLobbyServer(id, signal);
  if (signal.aborted) return;
  shown.value = fromOurs(result);
  loadedAt.value = now.value = Date.now();
  gone.value = false;
}

async function readNovaLogic(id: number, signal: AbortSignal): Promise<void> {
  const list = await fetchNovaLogicLobbies(signal);
  if (signal.aborted) return;
  now.value = Date.now();
  const fetched = list.ageSeconds !== null ? now.value - list.ageSeconds * 1000 : list.fetchedAt ? Date.parse(list.fetchedAt) : null;
  loadedAt.value = fetched !== null && Number.isFinite(fetched) ? fetched : null;
  unreachable.value = list.failing;
  // Before the service's first list, the server may yet be on it.
  awaitingList.value = list.enabled && list.fetchedAt === null;
  const found = list.servers.find((s) => s.id === id);
  if (found) {
    shown.value = fromNovaLogic(found);
    gone.value = false;
  } else if (!awaitingList.value) {
    gone.value = true;
  }
}

const poll = usePolling({
  async read(signal, background) {
    const id = Number(route.params.id);
    try {
      await (novalogic.value ? readNovaLogic(id, signal) : readOurs(id, signal));
      if (signal.aborted) return false;
      stale.value = false;
      error.value = '';
      return true;
    } catch (e) {
      if (signal.aborted) return false;
      now.value = Date.now();
      stale.value = true;
      if (e instanceof LobbyGone) {
        gone.value = true;
        return true;
      }
      if (!background || !shown.value) error.value = 'The server could not be read. Try again shortly.';
      return false;
    }
  },
  next: () => (gone.value ? null : POLL_MS),
});

// Whether this listing is the signed-in visitor's own: a hosted server of
// theirs opens its page, one they host themselves their Servers page.
async function findOwn() {
  manageTo.value = null;
  if (novalogic.value || !user.value) return;
  const id = Number(route.params.id);
  const mine = await listMyServers();
  if (!mine.ok || Number(route.params.id) !== id || novalogic.value) return;
  const row = mine.value.find((h) => h.rid === id);
  if (row) manageTo.value = row.hosted_server_id != null ? `/me/servers/${row.hosted_server_id}` : '/me/servers';
}

watch([() => route.path, () => user.value?.username], () => void findOwn(), { immediate: true });

watch(
  () => route.path,
  () => {
    shown.value = null;
    gone.value = false;
    loadedAt.value = null;
    stale.value = false;
    error.value = '';
    awaitingList.value = false;
    unreachable.value = false;
    void poll.refresh();
  },
);
</script>
