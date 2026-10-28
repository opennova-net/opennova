<template>
  <div class="space-y-6">
    <PageHeader title="Players" :tabs="PLAYERS_TABS" refreshable :loaded-at="loadedAt" :now="now" :loading="poll.loading.value" @refresh="refresh">
      <template v-if="online" #meta>
        <span class="online-count">{{ formatCount(online.count) }} online · {{ formatCount(playing) }} playing</span>
      </template>
    </PageHeader>

    <LiveNotices :message="message" :error="error" :poll-trouble="pollTrouble" />

    <div class="online-filters flex flex-wrap items-center gap-2">
      <label for="online-search" class="sr-only">Find a player</label>
      <input id="online-search" v-model="search" type="search" autocomplete="off" placeholder="Name or account" class="field w-auto" />
      <label for="online-game" class="sr-only">Game</label>
      <select id="online-game" v-model="game" class="field w-auto">
        <option value="">All games</option>
        <option v-for="g in games" :key="g.slug" :value="g.slug">{{ g.display_name }}</option>
      </select>
      <div role="group" aria-label="Where" class="flex gap-1">
        <button
          v-for="chip in STATE_CHIPS"
          :key="chip.value"
          type="button"
          class="btn-sm"
          :class="state === chip.value ? 'btn-secondary' : 'btn-ghost'"
          :aria-pressed="state === chip.value ? 'true' : 'false'"
          @click="state = chip.value"
        >
          {{ chip.label }}
        </button>
      </div>
    </div>

    <div class="table-wrap">
      <table class="data-table">
        <caption class="sr-only">Players online</caption>
        <thead>
          <tr>
            <th scope="col">Player</th>
            <th scope="col">Game</th>
            <th scope="col">Where</th>
            <th scope="col">Since</th>
            <th scope="col">Via</th>
            <th scope="col">Client</th>
            <th scope="col"><span class="sr-only">Actions</span></th>
          </tr>
        </thead>
        <tbody>
          <TableState
            v-if="shown.length === 0"
            :colspan="7"
            :rows="online?.players ?? null"
            :loading="poll.loading.value"
            :empty="online?.players.length ? 'No player matches.' : 'Nobody is online.'"
          />
          <tr v-for="p in shown" :key="rowKey(p)" class="online-row">
            <th scope="row">
              <PlayerLink :id="p.account?.id ?? null" :name="p.name || p.account?.username || 'Unknown'" /> <span
                v-if="p.account && p.account.username.toLowerCase() !== p.name.toLowerCase()"
                class="ml-1 text-xs font-normal text-ink-muted"
              >{{ p.account.username }}</span>
            </th>
            <td class="whitespace-nowrap">{{ p.game?.display_name ?? '' }}</td>
            <td>
              <template v-if="p.server">
                <ServerLink :name="p.server.name" :id="p.server.hosted_server_id" :rid="p.server.rid" />
                <span v-if="p.server.mission || gameTypeName(p.server.game_type)" class="online-map block text-xs text-ink-muted">
                  <GameText v-if="p.server.mission" :text="p.server.mission" />
                  <template v-if="p.server.mission && gameTypeName(p.server.game_type)"> · </template>
                  <GameText v-if="gameTypeName(p.server.game_type)" :text="gameTypeName(p.server.game_type)" />
                </span>
              </template>
              <span v-else class="text-ink-muted">In the lobby</span>
            </td>
            <td class="whitespace-nowrap text-ink-muted"><RelativeTime v-if="p.since_ms > 0" :value="p.since_ms" :now="now" /></td>
            <td><span v-if="p.via === 'novalogic'" class="badge">NovaLogic</span></td>
            <td class="text-xs text-ink-muted">{{ p.client ?? '' }}</td>
            <td class="text-right">
              <button v-if="p.session_id !== null" type="button" class="btn-ghost btn-sm" :disabled="busy" @click="endSession(p)">
                End session<span class="sr-only"> of {{ p.name }}</span>
              </button>
            </td>
          </tr>
        </tbody>
      </table>
    </div>
  </div>
</template>

<script setup lang="ts">
// /admin/players: who is on NovaWorld now (GET /api/admin/online, read every
// 15 s), each linked to their account and the server they play on, filtered
// in the URL query; an admin may end a player's lobby session.
import { computed, ref } from 'vue';
import GameText from '../../../components/GameText.vue';
import RelativeTime from '../../../components/RelativeTime.vue';
import PlayerLink from '../../../components/links/PlayerLink.vue';
import ServerLink from '../../../components/links/ServerLink.vue';
import LiveNotices from '../../../components/ui/LiveNotices.vue';
import PageHeader from '../../../components/ui/PageHeader.vue';
import TableState from '../../../components/ui/TableState.vue';
import { endConnection } from '../../../api/admin';
import { fetchOnline } from '../../../api/online';
import { confirmAction } from '../../../composables/useConfirm';
import { usePolling } from '../../../composables/usePolling';
import type { OnlinePlayer, OnlineResponse } from '../../../types/online';
import { PLAYERS_TABS } from '../../../utils/adminNav';
import { formatCount } from '../../../utils/format';
import { gameTypeName } from '../../../utils/games';
import { useQueryText } from '../../../composables/useQueryText';

const POLL_MS = 15_000;

const STATE_CHIPS = [
  { value: '', label: 'All' },
  { value: 'playing', label: 'Playing' },
  { value: 'lobby', label: 'In the lobby' },
] as const;

const online = ref<OnlineResponse | null>(null);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const message = ref('');
const error = ref('');
const pollTrouble = ref(false);
const busy = ref(false);

const search = useQueryText('q');
const game = useQueryText('game');
const state = useQueryText('state');

const playing = computed(() => (online.value?.players ?? []).filter((p) => p.state === 'playing').length);

// The games the rows play, once each, by name.
const games = computed(() => {
  const bySlug = new Map<string, string>();
  for (const p of online.value?.players ?? []) if (p.game) bySlug.set(p.game.slug, p.game.display_name);
  return [...bySlug].map(([slug, display_name]) => ({ slug, display_name })).sort((a, b) => a.display_name.localeCompare(b.display_name));
});

const shown = computed(() => {
  const q = search.value.trim().toLowerCase();
  return (online.value?.players ?? []).filter(
    (p) =>
      (!q || [p.name, p.account?.username, p.account?.nwhandle].some((v) => v?.toLowerCase().includes(q))) &&
      (!game.value || p.game?.slug === game.value) &&
      (!state.value || p.state === state.value),
  );
});

function rowKey(p: OnlinePlayer): string {
  return p.session_id !== null ? `s${p.session_id}` : `${p.via}:${p.server?.rid ?? ''}:${p.name}`;
}

const poll = usePolling({
  async read(signal, background) {
    const result = await fetchOnline(signal);
    if (signal.aborted) return false;
    now.value = Date.now();
    if (!result.ok) {
      if (background && online.value !== null) pollTrouble.value = true;
      else error.value = result.error.message;
      return false;
    }
    online.value = result.value;
    loadedAt.value = now.value;
    pollTrouble.value = false;
    return true;
  },
  next: () => POLL_MS,
});

function refresh() {
  error.value = '';
  void poll.refresh();
}

async function endSession(p: OnlinePlayer) {
  if (busy.value || p.session_id === null) return;
  if (!(await confirmAction(`End ${p.name}'s session? They are signed out of NovaWorld.`))) return;
  busy.value = true;
  const result = await endConnection(p.session_id);
  busy.value = false;
  if (result.ok) {
    message.value = `${p.name}'s session is ending.`;
    error.value = '';
  } else {
    error.value = result.error.message;
    message.value = '';
  }
  await poll.readAgain();
}
</script>
