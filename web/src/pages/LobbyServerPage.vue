<template>
  <section class="page py-10 sm:py-14">
    <RouterLink to="/lobby" class="text-sm text-accent hover:text-ink-bright">All lobbies</RouterLink>

    <p v-if="gone" class="mt-6 rounded-panel border border-border bg-panel p-6 text-sm text-ink-muted">
      This server is no longer listed.
    </p>
    <p v-else-if="error" role="alert" class="mt-6 rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger">
      {{ error }}
    </p>
    <p v-else-if="!server" class="mt-6 text-sm text-ink-muted">Loading…</p>

    <template v-if="server && !gone">
      <header class="mt-4 flex flex-wrap items-start justify-between gap-4">
        <div class="min-w-0">
          <p class="eyebrow">{{ server.game.displayName || 'NovaWorld' }}</p>
          <h1 class="mt-2 break-words text-3xl font-semibold text-ink-bright">{{ server.serverName || 'Unnamed server' }}</h1>
          <p v-if="message" class="mt-2 break-words text-sm text-ink-muted">{{ message }}</p>
        </div>
        <span class="shrink-0 rounded-control bg-hover px-3 py-1 text-sm font-semibold text-accent">
          {{ server.players }} / {{ server.maxPlayers || 0 }}
        </span>
      </header>

      <dl class="mt-6 grid gap-x-6 gap-y-2 rounded-panel border border-border bg-panel p-4 text-sm sm:grid-cols-[auto_1fr]">
        <template v-if="server.mission">
          <dt class="text-ink-muted">Mission</dt>
          <dd class="text-ink">{{ server.mission }}<span v-if="server.gameType" class="text-ink-muted"> ({{ server.gameType }})</span></dd>
        </template>
        <template v-if="server.timeLeft !== null">
          <dt class="text-ink-muted">Time left</dt>
          <dd class="text-ink">{{ server.timeLeft }} min</dd>
        </template>
        <template v-if="timeOfDay">
          <dt class="text-ink-muted">Time of day</dt>
          <dd class="text-ink">{{ timeOfDay }}</dd>
        </template>
        <template v-if="server.expansion">
          <dt class="text-ink-muted">Needs</dt>
          <dd class="text-ink">{{ server.expansion }}</dd>
        </template>
        <template v-if="server.locked">
          <dt class="text-ink-muted">Password</dt>
          <dd class="text-ink">Yes</dd>
        </template>
        <template v-if="server.hostIp">
          <dt class="text-ink-muted">Address</dt>
          <dd class="font-mono text-ink">{{ server.hostIp }}:{{ server.hostPort }}</dd>
        </template>
      </dl>

      <section class="mt-8 space-y-3" aria-labelledby="lobby-server-players">
        <h2 id="lobby-server-players" class="text-xl font-semibold text-ink">Players</h2>
        <div v-if="server.scoreboard && server.scoreboard.length > 0" class="relative overflow-x-auto rounded-panel border border-border">
          <table class="min-w-full divide-y divide-border text-sm text-ink">
            <thead>
              <tr class="bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted">
                <th scope="col" class="px-4 py-2">Name</th>
                <th scope="col" class="px-4 py-2 text-right">Team</th>
                <th scope="col" class="px-4 py-2 text-right">Kills</th>
                <th scope="col" class="px-4 py-2 text-right">Deaths</th>
              </tr>
            </thead>
            <tbody class="divide-y divide-border">
              <tr v-for="(p, i) in sortedScores" :key="`${i}-${p.name}`">
                <th scope="row" class="break-all px-4 py-2 text-left font-medium">{{ p.name }}</th>
                <td class="px-4 py-2 text-right font-mono text-xs">{{ p.team }}</td>
                <td class="px-4 py-2 text-right font-mono text-xs">{{ p.kills }}</td>
                <td class="px-4 py-2 text-right font-mono text-xs">{{ p.deaths }}</td>
              </tr>
            </tbody>
          </table>
        </div>
        <ul v-else-if="server.roster.length > 0" class="divide-y divide-border rounded-panel border border-border">
          <li v-for="(name, i) in server.roster" :key="`${i}-${name}`" class="break-all px-4 py-2 text-sm text-ink">{{ name }}</li>
        </ul>
        <p v-else class="text-sm text-ink-muted">No one is playing.</p>
      </section>
    </template>
  </section>
</template>

<script setup lang="ts">
// A listed server's page (GET /api/lobbies/<id>): its game info, and its
// players, with their scores on a server NovaWorld hosts. Read every 15 s.
import { computed, ref, watch } from 'vue';
import { RouterLink, useRoute } from 'vue-router';
import { fetchLobbyServer, LobbyGone } from '../api/lobbies';
import { usePolling } from '../composables/usePolling';
import type { LobbyServer } from '../types/lobby';

const POLL_MS = 15_000;
// The retail host screen's message until its host writes one.
const DEFAULT_MESSAGE = '!Put your message here';

const route = useRoute();
const server = ref<LobbyServer | null>(null);
const gone = ref(false);
const error = ref('');

const message = computed(() => {
  const text = server.value?.message?.trim() ?? '';
  return text === DEFAULT_MESSAGE ? '' : text;
});
const timeOfDay = computed(() => {
  const text = server.value?.timeOfDay ?? '';
  return text === 'Unknown' ? '' : text;
});
// Highest kills first.
const sortedScores = computed(() => [...(server.value?.scoreboard ?? [])].sort((a, b) => b.kills - a.kills || a.deaths - b.deaths));

const poll = usePolling({
  async read(signal, background) {
    try {
      server.value = await fetchLobbyServer(Number(route.params.id), signal);
      gone.value = false;
      error.value = '';
      return true;
    } catch (e) {
      if (signal.aborted) return false;
      if (e instanceof LobbyGone) {
        gone.value = true;
        return true;
      }
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
    void poll.refresh();
  },
);
</script>
