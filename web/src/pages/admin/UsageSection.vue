<template>
  <div class="space-y-10">
    <AdminPageHeader
      title="Server usage"
      description="Memory, CPU and network of each live hosted server, sampled every minute."
      :loaded-at="loadedAt"
      :now="now"
      :loading="poll.loading.value"
      @refresh="poll.refresh"
    />

    <div role="alert" :class="error ? 'rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger' : 'sr-only'">{{ error }}</div>
    <p v-if="pollTrouble" class="text-xs text-warn">The last refresh did not get through; this page keeps trying.</p>

    <dl v-if="usage" class="grid grid-cols-1 gap-3 min-[420px]:grid-cols-2 lg:grid-cols-3">
      <StatTile
        label="Live servers"
        :value="String(totals.servers_live)"
        :detail="`${plural(totals.players, 'player')} on them; ${totals.reporting} reporting their memory and CPU`"
      />
      <StatTile
        label="Machines"
        :value="String(totals.machines_live)"
        :detail="`${totals.slots_used} of ${plural(totals.slots, 'slot')} hold a server`"
      />
      <StatTile
        label="Memory in use"
        :value="formatMemory(totals.memory_bytes)"
        :detail="`Of ${formatMemory(totals.memory_limit_bytes)}, the reporting servers' limits added up`"
      />
      <StatTile
        label="CPU in use"
        :value="formatCpus(totals.cpus_used)"
        :detail="`Of ${formatCpus(totals.cpus_limit)}, the reporting servers' limits added up`"
      />
      <StatTile
        label="Sending to players now"
        :value="formatBitRate(totals.egress_bytes_per_second)"
        detail="All servers together, over each one's last sampled minute"
      />
      <StatTile
        :label="`Sent to players in the ${windowWords}`"
        :value="formatBytes(totals.egress_bytes_window)"
        detail="By the live servers"
      />
    </dl>

    <!-- Machines. -->
    <section class="space-y-4" aria-labelledby="usage-machines">
      <h2 id="usage-machines" class="text-xl font-semibold text-ink">Machines</h2>
      <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
        <table class="min-w-full divide-y divide-border">
          <caption class="sr-only">What the servers on each machine use</caption>
          <thead>
            <tr class="bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted">
              <th scope="col" class="px-4 py-3">Machine</th>
              <th scope="col" class="px-4 py-3">State</th>
              <th scope="col" class="px-4 py-3">Slots used</th>
              <th scope="col" class="px-4 py-3">Memory</th>
              <th scope="col" class="px-4 py-3">CPU</th>
              <th scope="col" class="px-4 py-3">Sending now</th>
              <th scope="col" class="px-4 py-3">Sent in the {{ windowWords }}</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border text-sm text-ink">
            <tr v-if="usage === null">
              <td colspan="7" class="px-4 py-6 text-center text-ink-muted">{{ poll.loading.value ? 'Loading…' : 'Not loaded.' }}</td>
            </tr>
            <tr v-else-if="usage.machines.length === 0">
              <td colspan="7" class="px-4 py-6 text-center text-ink-muted">No machine is running.</td>
            </tr>
            <tr v-for="m in usage?.machines ?? []" :key="m.id" class="usage-machine-row align-top hover:bg-hover">
              <th scope="row" class="whitespace-nowrap px-4 py-3 text-left font-normal">
                Machine {{ m.id }}
                <span class="block text-xs text-ink-muted">{{ machineKindLabel(m.kind) }}<template v-if="m.instance_type">, {{ m.instance_type }}</template></span>
              </th>
              <td class="whitespace-nowrap px-4 py-3">{{ machineStateLabel(m.state) }}</td>
              <td class="whitespace-nowrap px-4 py-3">{{ m.slots_used }} of {{ m.slots }}</td>
              <td class="whitespace-nowrap px-4 py-3">
                <template v-if="m.reporting > 0">{{ formatMemory(m.memory_bytes) }} of {{ formatMemory(m.memory_limit_bytes) }}</template>
                <span v-else class="text-ink-muted">No report</span>
              </td>
              <td class="whitespace-nowrap px-4 py-3">
                <template v-if="m.reporting > 0">{{ formatCpus(m.cpus_used) }} of {{ formatCpus(m.cpus_limit) }}</template>
                <span v-else class="text-ink-muted">No report</span>
              </td>
              <td class="whitespace-nowrap px-4 py-3">{{ formatBitRate(m.egress_bytes_per_second) }}</td>
              <td class="whitespace-nowrap px-4 py-3">{{ formatBytes(m.egress_bytes_window) }}</td>
            </tr>
          </tbody>
        </table>
      </div>
    </section>

    <!-- Servers. -->
    <section class="space-y-4" aria-labelledby="usage-servers">
      <div>
        <h2 id="usage-servers" class="text-xl font-semibold text-ink">Servers</h2>
        <p class="text-sm text-ink-muted">
          CPU is a share of the server's slot ({{ usage ? formatCpus(usage.slot_cpus) : 'its CPUs' }}). "No report":
          no sample in the last 3 minutes.
        </p>
      </div>
      <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
        <table class="min-w-full divide-y divide-border">
          <caption class="sr-only">What each live hosted server uses</caption>
          <thead>
            <tr class="bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted">
              <th scope="col" class="px-4 py-3">Server</th>
              <th scope="col" class="px-4 py-3">Owner</th>
              <th scope="col" class="px-4 py-3">Machine and slot</th>
              <th scope="col" class="px-4 py-3">State</th>
              <th scope="col" class="px-4 py-3">Players</th>
              <th scope="col" class="px-4 py-3">Memory</th>
              <th scope="col" class="px-4 py-3">CPU</th>
              <th scope="col" class="px-4 py-3">Sending</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border text-sm text-ink">
            <tr v-if="usage === null">
              <td colspan="8" class="px-4 py-6 text-center text-ink-muted">{{ poll.loading.value ? 'Loading…' : 'Not loaded.' }}</td>
            </tr>
            <tr v-else-if="usage.servers.length === 0">
              <td colspan="8" class="px-4 py-6 text-center text-ink-muted">No hosted server is live.</td>
            </tr>
            <tr v-for="s in usage?.servers ?? []" :key="s.id" class="usage-server-row align-top hover:bg-hover">
              <th scope="row" class="px-4 py-3 text-left font-medium">
                <RouterLink :to="`/admin/fleet/${s.id}`" class="break-words text-accent hover:text-ink-bright">
                  {{ s.name || 'Unnamed server' }}
                </RouterLink>
                <span class="block text-xs font-normal text-ink-muted">Server {{ s.id }}<template v-if="s.own_machine">, own machine</template></span>
              </th>
              <td class="whitespace-nowrap px-4 py-3">
                <template v-if="s.owner">{{ s.owner.username }}</template>
                <span v-else class="text-ink-muted">Deleted account</span>
              </td>
              <td class="whitespace-nowrap px-4 py-3">
                <template v-if="s.machine_ref">Machine {{ s.machine_ref }}, slot {{ s.slot ?? 'none' }}</template>
                <span v-else class="text-ink-muted">Not placed</span>
              </td>
              <td class="whitespace-nowrap px-4 py-3">
                <span :class="stateBadgeClass(s.state)">{{ stateLabel(s.state) }}</span>
                <span v-if="s.uptime_seconds !== null" class="block text-xs text-ink-muted">Up {{ formatDuration(s.uptime_seconds * 1000) }}</span>
              </td>
              <td class="whitespace-nowrap px-4 py-3">
                <template v-if="s.players !== null">{{ s.players }} of {{ s.max_players ?? '?' }}</template>
                <span v-else class="text-ink-muted">Not listed</span>
              </td>
              <td class="whitespace-nowrap px-4 py-3">
                <template v-if="s.memory_bytes !== null">{{ formatMemory(s.memory_bytes) }} of {{ formatMemory(slotMemory) }}</template>
                <span v-else class="text-ink-muted">No report</span>
                <span v-if="s.window.memory_bytes_peak !== null" class="block text-xs text-ink-muted">
                  Peak {{ formatMemory(s.window.memory_bytes_peak) }} in the {{ windowWords }}
                </span>
              </td>
              <td class="whitespace-nowrap px-4 py-3">
                <template v-if="s.cpu_percent !== null">{{ formatPercent(s.cpu_percent) }}</template>
                <span v-else class="text-ink-muted">No report</span>
                <span v-if="s.window.cpu_percent_mean !== null && s.window.cpu_percent_peak !== null" class="block text-xs text-ink-muted">
                  Mean {{ formatPercent(s.window.cpu_percent_mean) }}, peak {{ formatPercent(s.window.cpu_percent_peak) }}
                </span>
              </td>
              <td class="whitespace-nowrap px-4 py-3">
                <template v-if="s.egress_bytes_per_second !== null">{{ formatBitRate(s.egress_bytes_per_second) }}</template>
                <span v-else class="text-ink-muted">No report</span>
                <span class="block text-xs text-ink-muted">{{ formatBytes(s.window.egress_bytes) }} in the {{ windowWords }}</span>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </section>
  </div>
</template>

<script setup lang="ts">
// /admin/usage: what every live hosted server uses of its slot (GET
// /api/admin/hosted/usage), from the samples the service takes once a minute
// of what each machine's agent reports: the fleet's totals as tiles (live
// servers and their players, machines and their slots, memory and CPU in use
// against the reporting servers' limits, the rate sent to players now and the
// bytes sent in the last hour), each live machine's share, and each live
// server by machine and slot with its owner, state, players, memory, CPU and
// sending rate now and over the last hour. Read again every 30 s.
import AdminPageHeader from '../../components/AdminPageHeader.vue';
import { computed, ref } from 'vue';
import { RouterLink } from 'vue-router';
import StatTile from '../../components/charts/StatTile.vue';
import { fetchFleetUsage } from '../../api/hostedStats';
import { usePolling } from '../../composables/usePolling';
import type { FleetUsage, FleetUsageTotals } from '../../types/hostedStats';
import { machineKindLabel, machineStateLabel } from '../../utils/adminView';
import { formatBitRate, formatBytes, formatDuration, formatMemory, formatPercent } from '../../utils/format';
import { stateBadgeClass, stateLabel } from '../../utils/hostedState';

const POLL_MS = 30_000;


const usage = ref<FleetUsage | null>(null);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
const error = ref('');
const pollTrouble = ref(false);

const EMPTY_TOTALS: FleetUsageTotals = {
  servers_live: 0,
  machines_live: 0,
  slots: 0,
  slots_used: 0,
  reporting: 0,
  players: 0,
  memory_bytes: 0,
  memory_limit_bytes: 0,
  cpus_used: 0,
  cpus_limit: 0,
  egress_bytes_per_second: 0,
  egress_bytes_window: 0,
};
const totals = computed(() => usage.value?.totals ?? EMPTY_TOTALS);
const slotMemory = computed(() => usage.value?.slot_memory_bytes ?? 0);

// "last hour", or the window the service names in words.
const windowWords = computed(() => {
  const seconds = usage.value?.window_seconds ?? 3600;
  return seconds === 3600 ? 'last hour' : `last ${formatDuration(seconds * 1000)}`;
});

function plural(n: number, unit: string): string {
  return `${n} ${unit}${n === 1 ? '' : 's'}`;
}

// "0.45 vCPU", "1 vCPU", "12.5 vCPU": CPUs in use or their limits, in the unit
// the charts and ECS size them in (utils/format.ts formatCpus, which reads
// none in use as unknown: here it is a reading).
function formatCpus(cpus: number): string {
  const shown = cpus < 10 ? Math.round(cpus * 100) / 100 : Math.round(cpus * 10) / 10;
  return `${shown} vCPU`;
}

const poll = usePolling({
  async read(signal, background) {
    const result = await fetchFleetUsage(signal);
    if (signal.aborted) return false;
    now.value = Date.now();
    if (result.ok) {
      usage.value = result.value;
      loadedAt.value = now.value;
      pollTrouble.value = false;
      error.value = '';
      return true;
    }
    if (background && usage.value !== null) pollTrouble.value = true;
    else error.value = result.error.message;
    return false;
  },
  next: () => POLL_MS,
});
</script>
