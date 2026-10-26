<template>
  <p v-if="rows.length === 0 && !directory" class="text-sm text-ink-muted">{{ empty }}</p>
  <div v-else class="space-y-3">
    <div class="server-filters card flex flex-wrap items-end gap-x-4 gap-y-3 p-4 text-sm">
      <label class="min-w-0 flex-1 space-y-1 sm:max-w-xs">
        <span class="block text-xs font-medium text-ink-muted">Search</span>
        <input
          v-model="query"
          type="search"
          autocomplete="off"
          spellcheck="false"
          placeholder="Server, mission or game type"
          class="server-search w-full rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink focus:border-accent focus:outline-none"
        />
      </label>
      <label v-if="directory" class="space-y-1">
        <span class="block text-xs font-medium text-ink-muted">Game</span>
        <select v-model="game" class="server-game max-w-full rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink">
          <option value="">All games</option>
          <option v-for="item in games" :key="item.id" :value="item.id">{{ item.name }}</option>
        </select>
      </label>
      <label v-if="directory" class="space-y-1">
        <span class="block text-xs font-medium text-ink-muted">Listed on</span>
        <select v-model="source" class="server-source rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink">
          <option value="">Both services</option>
          <option value="opennova">OpenNova</option>
          <option value="novalogic">NovaLogic</option>
        </select>
      </label>
      <label class="space-y-1">
        <span class="block text-xs font-medium text-ink-muted">Game type</span>
        <select v-model="gameType" class="server-game-type rounded-control border border-border bg-surface px-3 py-2 text-sm text-ink">
          <option value="">All</option>
          <option v-for="t in gameTypes" :key="t" :value="t">{{ t }}</option>
        </select>
      </label>
      <label class="flex items-center gap-2 py-2 text-ink">
        <input v-model="hideEmpty" type="checkbox" class="server-hide-empty h-4 w-4 accent-accent" />
        Hide empty
      </label>
      <label class="flex items-center gap-2 py-2 text-ink">
        <input v-model="hideFull" type="checkbox" class="server-hide-full h-4 w-4 accent-accent" />
        Hide full
      </label>
      <label class="flex items-center gap-2 py-2 text-ink">
        <input v-model="hidePassworded" type="checkbox" class="server-hide-passworded h-4 w-4 accent-accent" />
        Hide passworded
      </label>
      <button v-if="filtering" type="button" class="server-clear py-2 text-sm text-accent hover:text-ink-bright" @click="clearFilters">Clear filters</button>
    </div>
    <p v-if="filtering || directory" class="server-count text-xs text-ink-muted" role="status">
      {{ shown.length }} of {{ rows.length }} {{ rows.length === 1 ? 'server' : 'servers' }}
    </p>

    <label class="flex items-center gap-3 text-sm text-ink-muted md:hidden">Sort by
      <select v-model="mobileSort" class="rounded-control border border-border bg-surface px-3 py-2 text-ink">
        <template v-for="col in COLUMNS" :key="col.key">
          <option :value="`${col.key}:asc`">{{ col.label }}: {{ col.key === 'players' ? 'fewest first' : 'A to Z' }}</option>
          <option :value="`${col.key}:desc`">{{ col.label }}: {{ col.key === 'players' ? 'most first' : 'Z to A' }}</option>
        </template>
      </select>
    </label>
    <ul class="server-mobile space-y-3 md:hidden" :aria-label="caption || 'Servers'">
      <li v-if="shown.length === 0" class="card p-6 text-sm text-ink-muted">{{ rows.length ? 'No server matches these filters.' : empty }}</li>
      <li v-for="row in shown" :key="row.key" class="card p-4">
        <div class="flex items-start justify-between gap-3">
          <RouterLink :to="row.to" class="min-w-0 break-words font-semibold text-ink hover:underline"><GameText :text="nameOf(row)" /></RouterLink>
          <span class="shrink-0 font-mono text-sm text-accent">{{ row.players }} / {{ row.maxPlayers }}<span class="sr-only"> players</span></span>
        </div>
        <p v-if="row.gameName || row.source" class="mt-1 text-xs text-ink-muted">
          {{ row.gameName }}<template v-if="row.source"> · {{ row.source === 'opennova' ? 'OpenNova' : 'NovaLogic' }}</template>
        </p>
        <p class="mt-3 break-words text-sm text-ink-muted">
          <GameText :text="row.mission || 'Mission not reported'" /><span v-if="row.gameType"> · <GameText :text="row.gameType" /></span>
        </p>
        <p v-if="row.country || row.expansion || row.password || row.locked || row.punkBuster" class="mt-3 flex flex-wrap gap-2 text-xs text-ink-muted">
          <span v-if="row.country">{{ row.country }}</span>
          <span v-if="row.expansion">{{ row.expansion }}</span>
          <span v-if="row.password">Password</span>
          <span v-else-if="row.locked">Locked</span>
          <span v-if="row.punkBuster">PunkBuster</span>
        </p>
      </li>
    </ul>

    <div class="relative hidden overflow-x-auto rounded-panel border border-border bg-panel md:block">
      <table class="min-w-full divide-y divide-border text-sm text-ink">
        <caption v-if="caption" class="sr-only">{{ caption }}</caption>
        <thead>
          <tr class="bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted">
            <th
              v-for="col in COLUMNS"
              :key="col.key"
              scope="col"
              class="whitespace-nowrap px-4 py-2"
              :class="{ 'text-right': col.key === 'players' }"
              :aria-sort="ariaSort(col.key)"
            >
              <button type="button" class="inline-flex items-center gap-1 uppercase tracking-wider hover:text-ink" @click="sortBy(col.key)">
                {{ col.label }}<span aria-hidden="true">{{ sortMark(col.key) }}</span>
              </button>
            </th>
          </tr>
        </thead>
        <tbody class="divide-y divide-border">
          <tr v-if="shown.length === 0">
            <td :colspan="COLUMNS.length" class="px-4 py-6 text-center text-ink-muted">{{ rows.length ? 'No server matches these filters.' : empty }}</td>
          </tr>
          <tr v-for="row in shown" :key="row.key" class="server-row cursor-pointer align-top hover:bg-hover" @click="open(row, $event)">
            <th scope="row" class="px-4 py-2 text-left font-medium">
              <RouterLink :to="row.to" class="break-words hover:underline"><GameText :text="nameOf(row)" /></RouterLink>
              <span v-if="row.password" class="ml-2 whitespace-nowrap rounded-control bg-hover px-2 py-0.5 text-xs font-normal text-ink-muted">Password</span>
              <span v-else-if="row.locked" class="ml-2 whitespace-nowrap rounded-control bg-hover px-2 py-0.5 text-xs font-normal text-ink-muted">Locked</span>
              <span v-if="row.punkBuster" class="ml-2 whitespace-nowrap rounded-control bg-hover px-2 py-0.5 text-xs font-normal text-ink-muted">PunkBuster</span>
              <p v-if="row.gameName || row.source" class="mt-1 text-xs font-normal text-ink-muted">{{ row.gameName }}<span v-if="row.source" class="ml-2 text-accent">{{ row.source === 'opennova' ? 'OpenNova' : 'NovaLogic' }}</span></p>
            </th>
            <td class="px-4 py-2">
              <GameText :text="row.mission" /><span v-if="row.mission && row.gameType" class="text-ink-muted"> (<GameText :text="row.gameType" />)</span
              ><GameText v-else-if="row.gameType" :text="row.gameType" />
            </td>
            <td class="whitespace-nowrap px-4 py-2 text-right font-mono text-xs">{{ row.players }} / {{ row.maxPlayers }}</td>
            <td class="px-4 py-2">{{ row.country }}</td>
            <td class="px-4 py-2">{{ row.expansion }}</td>
          </tr>
        </tbody>
      </table>
    </div>
  </div>
</template>

<script setup lang="ts">
// The Lobbies page's server table, for our NovaWorld's games and NovaLogic's
// list alike: each row opens its server's page; the headers sort (most players
// first until one is picked); a search, a game type and three switches filter.
// Names, missions and game types draw in the game's markup and sort and match
// by their visible text (utils/gameText.ts).
import { computed, ref } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import GameText from './GameText.vue';
import { stripGameText } from '../utils/gameText';
import type { ServerRow } from '../types/lobby';

const props = defineProps<{
  rows: ServerRow[];
  // What it says with no rows at all.
  empty: string;
  // The table's caption, for a screen reader.
  caption?: string;
  // One public directory owns its filters and sort order in the URL.
  directory?: boolean;
}>();

type SortKey = 'server' | 'mission' | 'players' | 'country' | 'expansion';

const COLUMNS: { key: SortKey; label: string }[] = [
  { key: 'server', label: 'Server' },
  { key: 'mission', label: 'Mission' },
  { key: 'players', label: 'Players' },
  { key: 'country', label: 'Country' },
  { key: 'expansion', label: 'Expansion' },
];

const router = useRouter();
const route = useRoute();

function textFilter(key: string) {
  const local = ref('');
  return computed({
    get: () => props.directory ? (typeof route.query[key] === 'string' ? route.query[key] as string : '') : local.value,
    set: (value: string) => {
      if (props.directory) void router.replace({ query: { ...route.query, [key]: value || undefined }, hash: route.hash });
      else local.value = value;
    },
  });
}
function flagFilter(key: string) {
  const value = textFilter(key);
  return computed({ get: () => value.value === '1', set: (on: boolean) => { value.value = on ? '1' : ''; } });
}
const query = textFilter('q');
const game = textFilter('game');
const source = textFilter('source');
const gameType = textFilter('type');
const hideEmpty = flagFilter('hideEmpty');
const hideFull = flagFilter('hideFull');
const hidePassworded = flagFilter('hidePassworded');
const localSort = ref<SortKey>('players');
const localAscending = ref(false);
const sortKey = computed<SortKey>(() => {
  if (!props.directory) return localSort.value;
  return COLUMNS.some((c) => c.key === route.query.sort) ? route.query.sort as SortKey : 'players';
});
const ascending = computed(() => props.directory ? route.query.order === 'asc' : localAscending.value);
const mobileSort = computed({
  get: () => `${sortKey.value}:${ascending.value ? 'asc' : 'desc'}`,
  set: (value: string) => {
    const [key, order] = value.split(':');
    setSort(key as SortKey, order === 'asc');
  },
});
const games = computed(() => {
  const found = new Map(props.rows.filter((row) => row.game).map((row) => [row.game!, row.gameName || row.game!]));
  if (game.value && !found.has(game.value)) found.set(game.value, game.value);
  return [...found].map(([id, name]) => ({ id, name })).sort((a, b) => compareText(a.name, b.name));
});
function clearFilters() {
  if (props.directory) {
    const next = { ...route.query };
    for (const key of ['q', 'game', 'source', 'type', 'hideEmpty', 'hideFull', 'hidePassworded']) delete next[key];
    void router.replace({ query: next, hash: route.hash });
  } else {
    query.value = ''; game.value = ''; source.value = ''; gameType.value = '';
    hideEmpty.value = false; hideFull.value = false; hidePassworded.value = false;
  }
}

// A row with its visible text, read once per list.
interface Prepared {
  row: ServerRow;
  name: string;
  mission: string;
  gameType: string;
}

const prepared = computed<Prepared[]>(() =>
  props.rows.map((row) => ({
    row,
    name: stripGameText(row.serverName).trim(),
    mission: stripGameText(row.mission).trim(),
    gameType: stripGameText(row.gameType).trim(),
  })),
);

// The game types the rows play, and the one picked even once no row plays it.
const gameTypes = computed(() => {
  const types = new Set(prepared.value.map((p) => p.gameType).filter((t) => t !== ''));
  if (gameType.value) types.add(gameType.value);
  return [...types].sort(compareText);
});

const filtering = computed(
  () => query.value.trim() !== '' || game.value !== '' || source.value !== '' || gameType.value !== '' || hideEmpty.value || hideFull.value || hidePassworded.value,
);

const shown = computed(() => {
  const needle = query.value.trim().toLocaleLowerCase();
  return prepared.value
    .filter((p) => {
      const { row } = p;
      if (game.value && row.game !== game.value) return false;
      if (source.value && row.source !== source.value) return false;
      if (hideEmpty.value && row.players <= 0) return false;
      if (hideFull.value && row.maxPlayers > 0 && row.players >= row.maxPlayers) return false;
      if (hidePassworded.value && (row.password || row.locked)) return false;
      if (gameType.value && p.gameType !== gameType.value) return false;
      if (needle && ![p.name, p.mission, p.gameType].some((t) => t.toLocaleLowerCase().includes(needle))) return false;
      return true;
    })
    .sort(compareRows)
    .map((p) => p.row);
});

function compareText(a: string, b: string): number {
  return a.localeCompare(b, undefined, { sensitivity: 'base', numeric: true });
}

// The picked column in its direction, then the name A to Z.
function compareRows(a: Prepared, b: Prepared): number {
  let order = 0;
  switch (sortKey.value) {
    case 'server':
      order = compareText(a.name, b.name);
      break;
    case 'mission':
      order = compareText(a.mission, b.mission) || compareText(a.gameType, b.gameType);
      break;
    case 'players':
      order = a.row.players - b.row.players;
      break;
    case 'country':
      order = compareText(a.row.country ?? '', b.row.country ?? '');
      break;
    case 'expansion':
      order = compareText(a.row.expansion ?? '', b.row.expansion ?? '');
      break;
  }
  if (!ascending.value) order = -order;
  return order || compareText(a.name, b.name);
}

// The same column again turns it around; another starts A to Z, or most
// players first.
function sortBy(key: SortKey) {
  setSort(key, sortKey.value === key ? !ascending.value : key !== 'players');
}

function setSort(key: SortKey, asc: boolean) {
  if (props.directory) {
    void router.replace({
      query: { ...route.query, sort: key === 'players' ? undefined : key, order: asc ? 'asc' : undefined },
      hash: route.hash,
    });
  } else {
    localSort.value = key;
    localAscending.value = asc;
  }
}

function ariaSort(key: SortKey): 'ascending' | 'descending' | undefined {
  if (key !== sortKey.value) return undefined;
  return ascending.value ? 'ascending' : 'descending';
}

function sortMark(key: SortKey): string {
  if (key !== sortKey.value) return '';
  return ascending.value ? ' ▲' : ' ▼';
}

function nameOf(row: ServerRow): string {
  return stripGameText(row.serverName).trim() ? row.serverName : 'Unnamed server';
}

// A click anywhere on a row opens its page, as its name's link does (which
// follows itself, a modified click into a new tab included); a click that
// selected text stays.
function open(row: ServerRow, event: MouseEvent) {
  if (event.target instanceof Element && event.target.closest('a')) return;
  if (window.getSelection()?.toString()) return;
  void router.push(row.to);
}
</script>
