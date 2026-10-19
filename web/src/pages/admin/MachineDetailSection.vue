<template>
  <div class="space-y-8">
    <RouterLink to="/admin/fleet" class="text-sm text-accent hover:text-ink-bright">Fleet</RouterLink>

    <div v-if="detail === null && !loadError" class="animate-pulse rounded-panel border border-border bg-panel p-8 text-center text-ink-muted">
      Loading the machine…
    </div>
    <div v-else-if="detail === null" class="space-y-3 rounded-panel border border-border bg-panel p-6">
      <h1 class="text-xl font-semibold text-ink">{{ missing ? 'No such machine' : 'The machine could not be loaded' }}</h1>
      <p role="alert" class="text-sm text-ink-muted">{{ loadError }}</p>
      <button v-if="!missing" type="button" :class="secondaryButton" @click="poll.refresh">Try again</button>
    </div>

    <template v-else>
      <header class="flex flex-col gap-3 sm:flex-row sm:items-start sm:justify-between">
        <div class="min-w-0 space-y-1">
          <h1 class="text-3xl font-semibold text-ink">Machine {{ machine!.id }}</h1>
          <p class="flex flex-wrap items-center gap-2 text-sm text-ink-muted">
            <span :class="toneBadgeClass(machineTone(machine!.state))">{{ machineStateLabel(machine!.state) }}</span>
            <span>{{ machineKindLabel(machine!.kind) }}</span>
            <span v-if="machine!.instance_type" class="font-mono text-xs">{{ machine!.instance_type }}</span>
            <span v-if="machine!.cloud_id" class="font-mono text-xs">{{ machine!.cloud_id }}</span>
          </p>
          <p v-if="report" class="machine-report text-sm" :class="report.late ? 'font-semibold text-danger' : 'text-ink-muted'">{{ report.text }}</p>
        </div>
        <div class="flex items-center gap-3">
          <p v-if="loadedAt" class="text-xs text-ink-muted">Updated <RelativeTime :value="loadedAt" :now="now" /></p>
          <button type="button" :class="secondaryButton" :disabled="poll.loading.value" @click="poll.refresh">
            {{ poll.loading.value ? 'Refreshing…' : 'Refresh' }}
          </button>
        </div>
      </header>
      <p v-if="pollTrouble" class="text-xs text-warn">The last refresh did not get through; this page keeps trying.</p>

      <ul v-if="flags.length" class="space-y-2">
        <li v-for="f in flags" :key="f.text" class="machine-flag rounded-panel border p-3 text-sm" :class="f.bad ? 'border-danger/40 bg-danger/10 text-danger' : 'border-warn/40 bg-warn/10 text-warn'">
          {{ f.text }}
        </li>
      </ul>

      <!-- Its facts against the release's. -->
      <section class="space-y-3" aria-labelledby="machine-facts">
        <h2 id="machine-facts" class="text-xl font-semibold text-ink">Facts</h2>
        <dl class="machine-facts grid gap-x-6 gap-y-2 rounded-panel border border-border bg-panel p-4 text-sm sm:grid-cols-[auto_1fr]">
          <dt class="text-ink-muted">hostd image</dt>
          <dd class="min-w-0">
            <span class="break-all font-mono text-xs text-ink">{{ machine!.agent_image ? shortImage(machine!.agent_image) : 'Not reported (not on ECS)' }}</span>
            <span class="block text-xs" :class="releaseWord(machine!.agent_image, machine!.release.agent_image).tone">
              {{ releaseWord(machine!.agent_image, machine!.release.agent_image).text }}
            </span>
          </dd>
          <dt class="text-ink-muted">Machine image</dt>
          <dd class="min-w-0">
            <span class="font-mono text-xs text-ink">{{ machine!.image_id || 'Not reported (not on EC2)' }}</span>
            <span class="block text-xs" :class="releaseWord(machine!.image_id, machine!.release.image_id).tone">
              {{ releaseWord(machine!.image_id, machine!.release.image_id).text }}
            </span>
          </dd>
          <dt class="text-ink-muted">hostd</dt>
          <dd class="text-ink">
            {{ machine!.agent_version ? `Version ${machine!.agent_version}` : 'No version reported' }}<template v-if="machine!.agent_protocol !== null">, protocol {{ machine!.agent_protocol }}</template>
          </dd>
          <dt class="text-ink-muted">Log stream</dt>
          <dd class="break-all font-mono text-xs text-ink">{{ machine!.agent_log_stream || 'None reported' }}</dd>
          <dt class="text-ink-muted">Address</dt>
          <dd class="text-ink">
            <span class="font-mono text-xs">{{ machine!.public_address || 'None yet' }}</span>
            <template v-if="machine!.address">, the static address <span class="font-mono text-xs">{{ machine!.address.name }}</span></template>
          </dd>
          <dt class="text-ink-muted">Launched</dt>
          <dd class="text-ink">
            <RelativeTime v-if="machine!.launched_at" :value="machine!.launched_at" :now="now" /><template v-else>Not yet</template>
            <template v-if="machine!.ready_at">, ready <RelativeTime :value="machine!.ready_at" :now="now" /></template>
          </dd>
          <template v-if="machine!.ended_at">
            <dt class="text-ink-muted">Ended</dt>
            <dd class="text-ink"><RelativeTime :value="machine!.ended_at" :now="now" /><template v-if="machine!.end_reason">: {{ machine!.end_reason }}</template></dd>
          </template>
          <dt class="text-ink-muted">Data sent this month</dt>
          <dd class="text-ink">{{ formatBytes(machine!.egress_bytes_month) }}, all its servers together</dd>
        </dl>
      </section>

      <!-- Its slots and the servers in them. -->
      <section v-if="live" class="space-y-3" aria-labelledby="machine-slots">
        <h2 id="machine-slots" class="text-xl font-semibold text-ink">Slots</h2>
        <p class="text-sm text-ink-muted">{{ machine!.slots_used }} of {{ machine!.slots }} in use. Slot k answers UDP 32768+k.</p>
        <SlotStrip :machine-id="machine!.id" :slots="machine!.slots" :holders="machine!.slot_holders" />
        <ul v-if="slotRows.length" class="divide-y divide-border rounded-panel border border-border bg-panel text-sm">
          <li v-for="r in slotRows" :key="r.slot" class="machine-server flex flex-wrap items-center justify-between gap-2 px-4 py-2">
            <RouterLink :to="`/admin/fleet/${r.server_id}`" class="text-accent hover:text-ink-bright">{{ r.name }}</RouterLink>
            <span class="text-xs text-ink-muted">
              Slot {{ r.slot }}, port {{ r.port }}, {{ stateLabel(r.state).toLowerCase() }}<template v-if="r.late">, {{ r.late }} late ticks in its last minute</template>
            </span>
          </li>
        </ul>
      </section>

      <!-- The trees its agent holds. -->
      <section class="space-y-3" aria-labelledby="machine-trees">
        <h2 id="machine-trees" class="text-xl font-semibold text-ink">Game data</h2>
        <p v-if="detail.trees.length === 0" class="text-sm text-ink-muted">Its agent holds no tree.</p>
        <ul v-else class="space-y-2">
          <li v-for="t in detail.trees" :key="t.tree_id" class="machine-tree space-y-1 rounded-panel border border-border bg-panel p-3 text-sm">
            <p class="flex flex-wrap items-center justify-between gap-2">
              <span class="font-medium text-ink">Tree {{ t.tree_id }}</span>
              <span :class="toneBadgeClass(t.state === 'ready' ? 'ok' : t.state === 'failed' ? 'bad' : 'warn')">{{ TREE_STATES[t.state] ?? t.state }}</span>
            </p>
            <div
              v-if="treeFraction(t) !== null && t.state !== 'ready'"
              class="h-1.5 overflow-hidden rounded-full bg-hover"
              role="progressbar"
              :aria-valuenow="Math.round(treeFraction(t)! * 100)"
              aria-valuemin="0"
              aria-valuemax="100"
              :aria-label="`Tree ${t.tree_id} fetched`"
            >
              <div class="h-full bg-accent" :style="{ width: `${treeFraction(t)! * 100}%` }"></div>
            </div>
            <p class="break-words" :class="t.state === 'failed' ? 'text-danger' : 'text-ink'">{{ treeProgress(t) }}</p>
            <p v-if="t.reported_at" class="text-xs text-ink-muted">Reported <RelativeTime :value="t.reported_at" :now="now" /></p>
          </li>
        </ul>
      </section>

      <!-- What happened to it. -->
      <section class="space-y-3" aria-labelledby="machine-timeline">
        <h2 id="machine-timeline" class="text-xl font-semibold text-ink">Timeline</h2>
        <p v-if="detail.events.length === 0" class="text-sm text-ink-muted">No event yet.</p>
        <ol v-else class="space-y-1 rounded-panel border border-border bg-panel p-4 text-sm">
          <li v-for="(e, index) in detail.events" :key="index" class="machine-event flex flex-col gap-x-3 sm:flex-row">
            <RelativeTime :value="e.at" :now="now" class="shrink-0 text-xs text-ink-muted sm:w-28" />
            <span class="break-words text-ink">{{ eventText(e) }}</span>
          </li>
        </ol>
      </section>

      <UsageCharts
        id-base="machine-usage"
        subject="machine"
        :id="machine!.id"
        :name="`Machine ${machine!.id}`"
        :heading-level="2"
        :follow="live"
      />

      <LogViewer
        id-base="machine-log"
        subject="machine"
        :id="machine!.id"
        heading="hostd's log"
        :heading-level="2"
        :followable="live"
      />
    </template>
  </div>
</template>

<script setup lang="ts">
// /admin/fleet/machines/<id>: one fleet machine (GET
// /api/admin/fleet/machines/<id>): its state and last report, what is wrong
// in words, its facts against the release (hostd's image, the machine's
// image), its slots with the servers in them (its slot_holders, each naming its
// server, port and ticks: the page reads no fleet list), the trees its agent
// holds with their progress, its timeline (its events), its usage
// (UsageCharts, with its disk) and hostd's log (LogViewer). Polled every 10 s
// while it lives.
import { computed, ref, watch } from 'vue';
import { RouterLink, useRoute } from 'vue-router';
import LogViewer from '../../components/LogViewer.vue';
import RelativeTime from '../../components/RelativeTime.vue';
import SlotStrip from '../../components/SlotStrip.vue';
import UsageCharts from '../../components/UsageCharts.vue';
import { fetchMachine } from '../../api/admin';
import { usePolling } from '../../composables/usePolling';
import type { MachineDetail } from '../../types/hostedServer';
import { machineKindLabel, machineStateLabel } from '../../utils/adminView';
import { formatBytes } from '../../utils/format';
import { stateLabel } from '../../utils/hostedState';
import { shortImage, toneBadgeClass } from '../../utils/infraView';
import { eventText, machineFlags, machineLive, machineTone, reportAge, treeFraction, treeProgress } from '../../utils/machineView';

const POLL_MS = 10_000;
// Slot k answers UDP 32768+k.
const GAME_PORT_BASE = 32768;

const TREE_STATES: Record<string, string> = { fetching: 'Fetching', ready: 'Ready', failed: 'Failed' };

const secondaryButton =
  'rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40';

const route = useRoute();
const id = computed(() => String(route.params.id ?? ''));

const detail = ref<MachineDetail | null>(null);
const loadError = ref('');
const missing = ref(false);
const pollTrouble = ref(false);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());

const machine = computed(() => detail.value?.machine ?? null);
// Its slots that hold a server, by slot, each with its server's name, port and
// ticks as the machine's holders name them.
const slotRows = computed(() =>
  [...(machine.value?.slot_holders ?? [])]
    .sort((a, b) => a.slot - b.slot)
    .map((h) => ({
      slot: h.slot,
      server_id: h.server_id,
      state: h.state,
      name: h.name || `Server ${h.server_id}`,
      port: h.game_port ?? GAME_PORT_BASE + h.slot,
      late: h.tick?.late ?? 0,
    })),
);
const live = computed(() => (machine.value ? machineLive(machine.value.state) : false));
const report = computed(() => (machine.value ? reportAge(machine.value, now.value) : null));

const flags = computed(() => (machine.value ? machineFlags(machine.value, now.value) : []));

// A fact against the release's: the same, another, or not known.
function releaseWord(own: string | null, release: string | null): { text: string; tone: string } {
  if (!release) return { text: 'The release pins none here, so it cannot be compared.', tone: 'text-ink-muted' };
  if (!own) return { text: `Not reported; the release pins ${shortImage(release)}.`, tone: 'text-ink-muted' };
  if (own === release) return { text: "The release's.", tone: 'text-online' };
  return { text: `Not the release's: it pins ${shortImage(release)}.`, tone: 'text-warn' };
}

const poll = usePolling({
  async read(signal, background) {
    if (!/^\d+$/.test(id.value)) {
      missing.value = true;
      loadError.value = 'There is no machine at this address.';
      return false;
    }
    const result = await fetchMachine(id.value, signal);
    if (signal.aborted) return false;
    now.value = Date.now();
    if (result.ok) {
      detail.value = result.value;
      loadedAt.value = now.value;
      loadError.value = '';
      pollTrouble.value = false;
      return true;
    }
    if (result.error.code === 'not_found') {
      missing.value = true;
      detail.value = null;
      loadError.value = 'No fleet machine has this id (an ended machine is kept 14 days).';
      return false;
    }
    if (background && detail.value !== null) pollTrouble.value = true;
    else loadError.value = result.error.message;
    return false;
  },
  next: () => (missing.value || !live.value ? null : POLL_MS),
});

// Another machine's page in the same place: start over.
watch(id, () => {
  detail.value = null;
  missing.value = false;
  loadError.value = '';
  void poll.refresh();
});
</script>
