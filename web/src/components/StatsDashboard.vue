<template>
  <section class="space-y-4" :aria-labelledby="`${baseId}-heading`">
    <div class="flex flex-wrap items-center justify-between gap-3">
      <div>
        <component :is="`h${headingLevel}`" :id="`${baseId}-heading`" class="section-heading">{{ heading }}</component>
        <p v-if="description" class="text-sm text-ink-muted">{{ description }}</p>
      </div>
      <!-- One row above everything it scopes. -->
      <RangePicker v-model="range" :base-id="`${baseId}-range`" />
    </div>

    <LiveNotices :error="error" />
    <p v-if="stats === null && !error" class="text-sm text-ink-muted">Loading…</p>

    <!-- A refetch keeps the last frame, dimmed. -->
    <div v-if="stats" class="space-y-5 transition-opacity" :class="refetching ? 'opacity-60' : ''">
      <!-- Right now, while the server is live. -->
      <section v-if="variant === 'server' && stats.live" class="card space-y-2 p-4" :aria-labelledby="`${baseId}-now`">
        <h3 :id="`${baseId}-now`" class="text-sm font-semibold text-ink">Now</h3>
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
            <dt class="text-ink-muted">Time left</dt>
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

      <LineChart
        :id="`${baseId}-players`"
        :title="variant === 'server' ? 'Players over time' : 'Players over time, all servers'"
        :description="variant === 'server' ? 'Gaps: not running.' : 'Gaps: no server running.'"
        :series="playerSeries(stats)"
        :start="span.start"
        :end="span.end"
        integer
        :format-value="formatCount"
      />
      <LineChart
        :id="`${baseId}-egress`"
        title="Data sent to players"
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
// The stats over a range (RangePicker): the headline tiles, and players and
// data sent over time. `variant` 'server' is one hosted server's (with what it
// does now, its availability and its CPU); 'aggregate' is several servers
// together (the account's, the fleet's). The page hands in the read (`load`);
// this polls it every minute while `follow`, and again when the range changes.
import { computed, ref, watch } from 'vue';
import LineChart from './charts/LineChart.vue';
import RangePicker from './charts/RangePicker.vue';
import Sparkline from './charts/Sparkline.vue';
import StatTile from './charts/StatTile.vue';
import LiveNotices from './ui/LiveNotices.vue';
import type { StatsResult } from '../api/hostedStats';
import { usePolling } from '../composables/usePolling';
import type { HostedStats, StatsRange } from '../types/hostedStats';
import { formatBytes, formatCount, formatDuration, formatFraction } from '../utils/format';
import { ownerStateLabel } from '../utils/hostedState';
import { RANGE_WORDS, cpuValues, egressSeries, playerSeries, playerValues, statsWindow } from '../utils/statsView';

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
  // An estimate of nothing is exactly nothing.
  const approx = (field: string, value: number) => value !== 0 && sum.approximate.includes(field);
  const out: Tile[] = [];
  if (props.variant === 'server') {
    const restarts = sum.restarts === 0 ? 'no restarts' : sum.restarts === 1 ? '1 restart' : `${sum.restarts} restarts`;
    out.push({
      label: 'Availability',
      value: sum.availability !== null ? formatFraction(sum.availability) : s.live ? 'Not yet' : 'Not live',
      detail:
        sum.live_seconds > 0
          ? `Running ${formatDuration(sum.running_seconds * 1000)} of ${formatDuration(sum.live_seconds * 1000)} live, ${restarts}`
          : s.live
            ? 'First sample within a minute'
            : `Not live ${words}`,
      approximate: approx('availability', sum.availability ?? 0),
    });
  }
  out.push({
    label: 'Peak players',
    value: formatCount(sum.peak_players),
    detail: sum.avg_players === null ? undefined : `Average ${Math.round(sum.avg_players * 10) / 10}`,
    approximate: approx('peak_players', sum.peak_players),
    trend: { values: playerValues(s), label: 'Players', format: formatCount },
  });
  out.push({
    label: 'Player-hours',
    value: (Math.round(sum.player_hours * 10) / 10).toLocaleString('en-US'),
    approximate: approx('player_hours', sum.player_hours),
  });
  out.push({
    label: 'Unique players',
    value: formatCount(sum.unique_players),
    approximate: approx('unique_players', sum.unique_players),
  });
  out.push({
    label: 'Missions played',
    value: formatCount(sum.missions),
    approximate: approx('missions', sum.missions),
  });
  out.push({
    label: 'Data sent',
    value: formatBytes(sum.egress_bytes),
    approximate: approx('egress_bytes', sum.egress_bytes),
  });
  if (props.variant === 'server') {
    const live = s.live;
    out.push({
      label: 'CPU',
      value: live?.cpu_percent != null ? percent(live.cpu_percent) : 'No report',
      detail: live?.memory_bytes != null ? `Memory ${formatBytes(live.memory_bytes)}` : live ? 'No report yet' : 'Not running',
      trend: { values: cpuValues(s), label: 'CPU', format: percent },
    });
  }
  return out;
});
</script>
