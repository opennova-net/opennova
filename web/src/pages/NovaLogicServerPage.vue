<template>
  <section class="page py-10 sm:py-14">
    <RouterLink to="/lobby" class="text-sm text-accent hover:text-ink-bright">All servers</RouterLink>

    <p v-if="gone" class="mt-6 rounded-panel border border-border bg-panel p-6 text-sm text-ink-muted">
      This server is no longer listed.
    </p>
    <p v-else-if="error" role="alert" class="mt-6 rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger">
      {{ error }}
    </p>
    <p v-else-if="!server && unreachable" class="mt-6 text-sm text-ink-muted">NovaLogic's NovaWorld can't be reached right now.</p>
    <p v-else-if="!server && awaitingList" class="mt-6 text-sm text-ink-muted">Checking NovaLogic's NovaWorld...</p>
    <p v-else-if="!server" class="mt-6 text-sm text-ink-muted">Loading…</p>

    <template v-if="server && !gone">
      <header class="mt-4 flex flex-wrap items-start justify-between gap-4">
        <div class="min-w-0">
          <p class="eyebrow">NovaLogic's NovaWorld</p>
          <h1 class="mt-2 break-words text-3xl font-semibold text-ink-bright"><GameText :text="name" /></h1>
          <p v-if="message" class="mt-2 whitespace-pre-line break-words text-sm text-ink-muted"><GameText :text="message" /></p>
        </div>
        <span class="shrink-0 rounded-control bg-hover px-3 py-1 text-sm font-semibold text-accent">
          {{ server.players }} / {{ server.maxPlayers || 0 }}
        </span>
      </header>

      <section class="mt-6 rounded-panel border border-accent/30 bg-accent/5 p-5" aria-labelledby="join-server">
        <h2 id="join-server" class="font-semibold text-ink-bright">Join through NovaLogic's NovaWorld</h2>
        <p class="mt-2 text-sm leading-relaxed text-ink-muted">To join, connect your game to NovaLogic's NovaWorld and find <span class="font-medium text-ink"><GameText :text="name" /></span> in the game's server browser. Use your NovaLogic account there. In OpenNova, choose Original NovaWorld; the original Jointops.exe needs no Switcher.</p>
        <p v-if="server.password" class="mt-2 text-sm text-warn">Ask the host for this server's password.</p>
        <p v-else-if="server.locked" class="mt-2 text-sm text-warn">This server is locked and may not accept new players.</p>
        <RouterLink to="/play?game=jointops#online" class="link mt-3 inline-block text-sm">Joint Operations setup instructions</RouterLink>
      </section>
      <p v-if="loadedAt !== null" class="mt-3 text-xs text-ink-muted">List updated <RelativeTime :value="loadedAt" :now="now" /><span v-if="stale" class="text-warn">. The last refresh failed; showing the last details received.</span></p>

      <p v-if="unreachable" class="mt-4 text-xs text-ink-muted">
        NovaLogic's NovaWorld can't be reached right now, so this is from the last list we got.
      </p>

      <dl class="mt-6 grid gap-x-6 gap-y-2 rounded-panel border border-border bg-panel p-4 text-sm sm:grid-cols-[auto_1fr]">
        <template v-if="server.mission || server.gameType">
          <dt class="text-ink-muted">Mission</dt>
          <dd class="text-ink">
            <GameText :text="server.mission" /><span v-if="server.mission && server.gameType" class="text-ink-muted"> (<GameText :text="server.gameType" />)</span
            ><GameText v-else-if="server.gameType" :text="server.gameType" />
          </dd>
        </template>
        <template v-if="timeLeft">
          <dt class="text-ink-muted">Time left</dt>
          <dd class="text-ink">{{ timeLeft }}</dd>
        </template>
        <template v-if="timeOfDay">
          <dt class="text-ink-muted">Time of day</dt>
          <dd class="text-ink">{{ timeOfDay }}</dd>
        </template>
        <template v-if="server.country">
          <dt class="text-ink-muted">Country</dt>
          <dd class="text-ink">{{ server.country }}</dd>
        </template>
        <template v-if="server.region">
          <dt class="text-ink-muted">Region</dt>
          <dd class="text-ink">{{ server.region }}</dd>
        </template>
        <template v-if="server.expansion">
          <dt class="text-ink-muted">Expansion</dt>
          <dd class="font-mono text-ink">{{ server.expansion }}</dd>
        </template>
        <dt class="text-ink-muted">Password</dt>
        <dd class="text-ink">{{ server.password ? 'Yes' : 'No' }}</dd>
        <dt class="text-ink-muted">Locked</dt>
        <dd class="text-ink">{{ server.locked ? 'Yes' : 'No' }}</dd>
        <dt class="text-ink-muted">PunkBuster</dt>
        <dd class="text-ink">{{ server.punkBuster ? 'Yes' : 'No' }}</dd>
        <dt class="text-ink-muted">Dedicated</dt>
        <dd class="text-ink">{{ server.dedicated ? 'Yes' : 'No' }}</dd>
        <dt class="text-ink-muted">Tracers</dt>
        <dd class="text-ink">{{ server.tracers ? 'On' : 'Off' }}</dd>
      </dl>

      <section class="mt-8 space-y-3" aria-labelledby="novalogic-server-players">
        <h2 id="novalogic-server-players" class="text-xl font-semibold text-ink">Players</h2>
        <ul v-if="server.roster.length > 0" class="divide-y divide-border rounded-panel border border-border">
          <li v-for="(player, i) in server.roster" :key="`${i}-${player}`" class="break-all px-4 py-2 text-sm text-ink">{{ player }}</li>
        </ul>
        <p v-else class="text-sm text-ink-muted">No one is playing.</p>
      </section>

    </template>
  </section>
</template>

<script setup lang="ts">
// A server on NovaLogic's NovaWorld (/lobby/novalogic/<id>): its row of the
// service's copy of NovaLogic's list (GET /api/lobbies/novalogic), with its
// players' names. Read every 15 s, as the list itself is fetched on a cadence.
import { computed, ref, watch } from 'vue';
import { RouterLink, useRoute } from 'vue-router';
import GameText from '../components/GameText.vue';
import RelativeTime from '../components/RelativeTime.vue';
import { fetchNovaLogicLobbies } from '../api/lobbies';
import { usePolling } from '../composables/usePolling';
import { stripGameText } from '../utils/gameText';
import type { NovaLogicServer } from '../types/lobby';

const POLL_MS = 15_000;
// The host screen's message until its host writes one: the menu's ("!Put your message here")
// or game.cfg's default ("Put your message here."), which a host file or a dedicated server
// leaves in.
const DEFAULT_MESSAGES = ['!Put your message here', 'Put your message here.'];

const route = useRoute();
const server = ref<NovaLogicServer | null>(null);
const gone = ref(false);
const error = ref('');
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const stale = ref(false);
// The service has no list yet (it fetches NovaLogic's on its own cadence).
const awaitingList = ref(false);
// The service's last fetch of NovaLogic's list failed.
const unreachable = ref(false);

const name = computed(() => {
  const text = server.value?.serverName ?? '';
  return stripGameText(text).trim() ? text : 'Unnamed server';
});
const message = computed(() => {
  const text = server.value?.message?.trim() ?? '';
  return DEFAULT_MESSAGES.includes(text) ? '' : text;
});
// Minutes as its host wrote them ("29"), or what else it wrote; "NA" is none.
const timeLeft = computed(() => {
  const text = server.value?.timeLeft?.trim() ?? '';
  if (text === '' || text.toUpperCase() === 'NA') return '';
  return /^\d+$/.test(text) ? `${text} min` : text;
});
const timeOfDay = computed(() => {
  const text = server.value?.timeOfDay?.trim() ?? '';
  return text === 'Unknown' ? '' : text;
});

const poll = usePolling({
  async read(signal, background) {
    try {
      const list = await fetchNovaLogicLobbies(signal);
      if (signal.aborted) return false;
      now.value = Date.now();
      const fetched = list.ageSeconds !== null ? now.value - list.ageSeconds * 1000 : list.fetchedAt ? Date.parse(list.fetchedAt) : null;
      loadedAt.value = fetched !== null && Number.isFinite(fetched) ? fetched : null;
      stale.value = false;
      const id = Number(route.params.id);
      const found = list.servers.find((s) => s.id === id) ?? null;
      unreachable.value = list.failing;
      error.value = '';
      // Before the service's first list, the server may yet be on it.
      awaitingList.value = list.enabled && list.fetchedAt === null;
      if (found) {
        server.value = found;
        gone.value = false;
      } else if (!awaitingList.value) {
        gone.value = true;
      }
      return true;
    } catch (e) {
      if (signal.aborted) return false;
      now.value = Date.now();
      stale.value = true;
      if (!background || !server.value) error.value = 'The server could not be read. Try again shortly.';
      return false;
    }
  },
  next: () => (gone.value ? null : POLL_MS),
});

watch(
  () => route.params.id,
  () => {
    server.value = null;
    gone.value = false;
    loadedAt.value = null;
    stale.value = false;
    error.value = '';
    void poll.refresh();
  },
);
</script>
