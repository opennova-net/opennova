<template>
  <section class="space-y-5" :aria-labelledby="`${baseId}-heading`">
    <div class="space-y-3">
      <div>
        <component :is="`h${headingLevel}`" :id="`${baseId}-heading`" class="text-lg font-semibold text-ink">{{ heading }}</component>
        <p v-if="description" class="text-sm text-ink-muted">{{ description }}</p>
      </div>
      <!-- One row above everything it scopes. -->
      <RangePicker v-model="range" :base-id="`${baseId}-range`" />
    </div>

    <p role="alert" class="sr-only">{{ error }}</p>
    <p v-if="error" aria-hidden="true" class="rounded-panel border border-danger/40 bg-danger/10 p-3 text-sm text-danger">{{ error }}</p>

    <div v-if="stats === null && !error" class="animate-pulse rounded-panel border border-border bg-panel p-6 text-center text-sm text-ink-muted">
      Loading the stats…
    </div>

    <!-- A refetch keeps the last frame, dimmed. -->
    <div v-if="stats" class="space-y-5 transition-opacity" :class="refetching ? 'opacity-60' : ''">
      <!-- Right now, while the server is live. -->
      <section v-if="variant === 'server' && stats.live" class="space-y-2 rounded-panel border border-border bg-panel p-4" :aria-labelledby="`${baseId}-now`">
        <h3 :id="`${baseId}-now`" class="text-sm font-semibold text-ink">Right now</h3>
        <dl class="grid gap-x-6 gap-y-1 text-sm sm:grid-cols-[auto_1fr]">
          <dt class="text-ink-muted">State</dt>
          <dd class="text-ink">{{ ownerStateLabel(stats.live.state) }}<template v-if="stats.live.uptime_seconds !== null">, up for {{ formatDuration(stats.live.uptime_seconds * 1000) }}</template></dd>
          <dt class="text-ink-muted">Players</dt>
          <dd class="text-ink">{{ stats.live.players === null ? 'Not listed' : `${stats.live.players} of ${stats.live.max_players ?? '?'}` }}</dd>
          <template v-if="stats.live.mission">
            <dt class="text-ink-muted">Mission</dt>
            <dd class="break-words text-ink">
              {{ stats.live.mission }}<template v-if="stats.live.game_type"> ({{ stats.live.game_type }})</template>
            </dd>
          </template>
          <template v-if="stats.live.time_left !== null">
            <dt class="text-ink-muted">Time left in the round</dt>
            <dd class="text-ink">{{ formatDuration(stats.live.time_left * 1000) }}</dd>
          </template>
          <template v-if="sceneText">
            <dt class="text-ink-muted">Scene</dt>
            <dd class="text-ink">{{ sceneText }}</dd>
          </template>
          <template v-if="stats.live.link_state">
            <dt class="text-ink-muted">Console link</dt>
            <dd class="text-ink">{{ linkText(stats.live.link_state) }}</dd>
          </template>
        </dl>
      </section>

      <dl class="grid grid-cols-1 gap-3 min-[420px]:grid-cols-2 lg:grid-cols-4">
        <StatTile
          v-for="tile in tiles"
          :key="tile.label"
          :label="tile.label"
          :value="tile.value"
          :detail="tile.detail"
          :approximate="tile.approximate"
        >
          <template v-if="tile.trend" #trend>
            <Sparkline :values="tile.trend.values" :label="tile.trend.label" :format="tile.trend.format" />
          </template>
        </StatTile>
      </dl>
      <p v-if="stats.summary.approximate.length > 0" class="text-xs text-ink-muted">
        Figures marked "about" are estimates: they add up hourly counts that may count a player twice.
      </p>

      <LineChart
        :id="`${baseId}-players`"
        :title="variant === 'server' ? 'Players over time' : 'Players over time, all servers together'"
        :description="`Players ${RANGE_WORDS[stats.range]}; ${stepWords(stats)}. Gaps are times ${variant === 'server' ? 'the server was not running' : 'no server was running'}.`"
        :series="playerSeries(stats)"
        :start="span.start"
        :end="span.end"
        integer
        :format-value="formatCount"
      />
      <LineChart
        :id="`${baseId}-egress`"
        title="Data sent to players"
        :description="`Bytes sent ${RANGE_WORDS[stats.range]}, in each period; ${stepWords(stats)}.`"
        :series="egressSeries(stats)"
        :start="span.start"
        :end="span.end"
        :min-top="1000"
        :format-value="formatBytes"
      />
    </div>
  </section>
</template>

<script setup lang="ts">
// The stats over a range (RangePicker, one row above everything it scopes):
// the headline tiles (StatTile), and players and data sent over time
// (LineChart, hand-written SVG). `variant` 'server' is one hosted server's
// (with what it is doing right now, its availability and its machine's CPU);
// 'aggregate' is several servers together (the account's, the fleet's). The
// page hands in the read (`load`); this polls it every minute while `follow`,
// and again at once when the range changes.
import { computed, ref, watch } from 'vue';
import LineChart from './charts/LineChart.vue';
import RangePicker from './charts/RangePicker.vue';
import Sparkline from './charts/Sparkline.vue';
import StatTile from './charts/StatTile.vue';
import type { StatsResult } from '../api/hostedStats';
import { usePolling } from '../composables/usePolling';
import type { HostedStats, StatsRange } from '../types/hostedStats';
import { formatBytes, formatCount, formatDuration, formatFraction } from '../utils/format';
import { ownerStateLabel } from '../utils/hostedState';
import { RANGE_WORDS, cpuValues, egressSeries, playerSeries, playerValues, statsWindow, stepWords } from '../utils/statsView';

const props = withDefaults(
  defineProps<{
    load: (range: StatsRange, signal: AbortSignal) => Promise<StatsResult<HostedStats>>;
    baseId: string;
    heading: string;
    description?: string;
    variant: 'server' | 'aggregate';
    // Poll every minute (the service samples each live server once a minute).
    follow?: boolean;
    initialRange?: StatsRange;
    headingLevel?: 2 | 3;
  }>(),
  { description: '', follow: true, initialRange: '24h', headingLevel: 2 },
);

const emit = defineEmits<{
  // The read was refused the session.
  (e: 'session-ended'): void;
}>();

const POLL_MS = 60_000;

interface Tile {
  label: string;
  value: string;
  detail?: string;
  approximate?: boolean;
  trend?: { values: (number | null)[]; label: string; format: (v: number) => string };
}

const range = ref<StatsRange>(props.initialRange);
const stats = ref<HostedStats | null>(null);
const error = ref('');
const now = ref(Date.now());

const poll = usePolling({
  async read(signal, background) {
    const result = await props.load(range.value, signal);
    if (signal.aborted) return false;
    now.value = Date.now();
    if (result.ok) {
      stats.value = result.value;
      error.value = '';
      return true;
    }
    if (result.error.code === 'unauthorized' || result.error.code === 'forbidden') emit('session-ended');
    if (!background || stats.value === null) error.value = result.error.message;
    return false;
  },
  next: () => (props.follow ? POLL_MS : null),
});

watch(range, () => void poll.refresh());

// A read in flight that someone asked for (a refresh, a new range).
const refetching = poll.loading;

const span = computed(() => (stats.value ? statsWindow(stats.value, now.value) : { start: now.value, end: now.value }));

const sceneText = computed(() => {
  switch (stats.value?.live?.scene) {
    case 'game':
      return 'In a match';
    case 'menus':
      return 'Between matches';
    default:
      return '';
  }
});

function linkText(state: string): string {
  switch (state) {
    case 'up':
      return 'Up';
    case 'connecting':
      return 'Connecting';
    case 'down':
      return 'Down, reconnecting';
    case 'stopped':
      return 'Closed';
    default:
      return state;
  }
}

const percent = (v: number) => `${Math.round(v)}%`;

const tiles = computed<Tile[]>(() => {
  const s = stats.value;
  if (!s) return [];
  const sum = s.summary;
  const words = RANGE_WORDS[s.range];
  const approx = (field: string) => sum.approximate.includes(field);
  const out: Tile[] = [];
  if (props.variant === 'server') {
    const restarts = sum.restarts === 0 ? 'no restarts' : sum.restarts === 1 ? '1 restart' : `${sum.restarts} restarts`;
    out.push({
      label: 'Availability',
      value: sum.availability === null ? 'Not live' : formatFraction(sum.availability),
      detail:
        sum.live_seconds > 0
          ? `Running ${formatDuration(sum.running_seconds * 1000)} of the ${formatDuration(sum.live_seconds * 1000)} it was live, ${restarts}`
          : `Not live ${words}`,
      approximate: approx('availability'),
    });
  }
  out.push({
    label: 'Peak players',
    value: formatCount(sum.peak_players),
    detail: sum.avg_players === null ? `No players sampled ${words}` : `Average ${Math.round(sum.avg_players * 10) / 10} ${words}`,
    approximate: approx('peak_players'),
    trend: { values: playerValues(s), label: 'Players', format: formatCount },
  });
  out.push({
    label: 'Player-hours',
    value: (Math.round(sum.player_hours * 10) / 10).toLocaleString('en-US'),
    detail: `Hours played by all players together ${words}`,
    approximate: approx('player_hours'),
  });
  out.push({
    label: 'Unique players',
    value: formatCount(sum.unique_players),
    detail: `Different players ${words}`,
    approximate: approx('unique_players'),
  });
  out.push({
    label: 'Missions played',
    value: formatCount(sum.missions),
    detail: `Maps started ${words}`,
    approximate: approx('missions'),
  });
  out.push({
    label: 'Data sent',
    value: formatBytes(sum.egress_bytes),
    detail: `To players ${words}`,
    approximate: approx('egress_bytes'),
  });
  if (props.variant === 'server') {
    const live = s.live;
    out.push({
      label: 'CPU',
      value: live?.cpu_percent != null ? percent(live.cpu_percent) : 'No report',
      detail:
        live?.memory_bytes != null
          ? `Memory ${formatBytes(live.memory_bytes)}, as the machine last reported`
          : live
            ? 'The machine has not reported yet'
            : 'Not running',
      trend: { values: cpuValues(s), label: 'CPU', format: percent },
    });
  }
  return out;
});
</script>
