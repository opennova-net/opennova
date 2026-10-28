<template>
  <div class="space-y-6">
    <PageHeader :title="`Machine ${id}`" :crumbs="CRUMBS" refreshable :loaded-at="loadedAt" :now="now" :loading="poll.loading.value" @refresh="poll.refresh">
      <template v-if="machine" #meta>
        <Badge :tone="machineTone(machine.state)">{{ machineStateLabel(machine.state) }}</Badge>
        <span>{{ machineKindLabel(machine.kind) }}</span>
        <span v-if="machine.instance_type" class="font-mono text-xs">{{ machine.instance_type }}</span>
        <span v-if="report" class="machine-report" :class="report.late ? 'font-semibold text-danger' : ''">{{ report.text }}</span>
      </template>
      <template v-if="machine && (canDrain || canEnd)" #actions>
        <button v-if="canDrain" type="button" class="btn-secondary" :disabled="busy" @click="onAction('drain')">Drain…</button>
        <button v-if="canEnd" type="button" class="btn-danger" :disabled="busy" @click="onAction('end')">End…</button>
      </template>
    </PageHeader>

    <LiveNotices :message="message" :error="error" :poll-trouble="pollTrouble" />
    <p v-if="detail === null && !error" class="text-sm text-ink-muted">Loading…</p>

    <template v-if="detail && machine">
      <Notice v-for="f in flags" :key="f.text" :tone="f.bad ? 'bad' : 'warn'" class="machine-flag">{{ f.text }}</Notice>

      <dl class="machine-tiles grid grid-cols-1 gap-3 min-[420px]:grid-cols-2 lg:grid-cols-3 2xl:grid-cols-6">
        <StatTile label="Slots" :value="`${machine.slots_used}/${machine.slots}`" />
        <StatTile v-for="t in tiles" :key="t.label" :label="t.label" :value="t.value" :detail="t.detail" :tone="t.flag ? 'warn' : undefined" />
        <StatTile label="Sent this month" :value="formatBytes(machine.egress_bytes_month)" />
      </dl>

      <section v-if="live" class="space-y-2" aria-labelledby="machine-slots">
        <h2 id="machine-slots" class="section-heading">Slots</h2>
        <div class="table-wrap">
          <table class="data-table">
            <thead>
              <tr>
                <th scope="col">Slot</th>
                <th scope="col">Server</th>
                <th scope="col">Game</th>
                <th scope="col">Players</th>
                <th scope="col">Owner</th>
                <th scope="col">State</th>
                <th scope="col">Late ticks</th>
              </tr>
            </thead>
            <tbody>
              <TableState v-if="slotRows.length === 0" :colspan="7" :rows="slotRows" empty="No servers." />
              <tr v-for="r in slotRows" :key="r.slot" class="machine-server">
                <td class="font-mono">{{ r.slot }}</td>
                <th scope="row"><ServerLink :name="r.name" :id="r.serverId" scope="admin" /></th>
                <td>{{ r.game || '—' }}</td>
                <td>{{ r.players || '—' }}</td>
                <td>
                  <PlayerLink v-if="r.owner" :id="r.owner.id" :name="r.owner.username" />
                  <span v-else class="text-ink-muted">—</span>
                </td>
                <td><Badge :tone="serverStateTone(r.state)">{{ stateLabel(r.state) }}</Badge></td>
                <td :class="r.late ? 'font-semibold text-warn' : 'text-ink-muted'">{{ r.late ?? '—' }}</td>
              </tr>
            </tbody>
          </table>
        </div>
      </section>

      <section class="space-y-2" aria-labelledby="machine-trees">
        <h2 id="machine-trees" class="section-heading">Games</h2>
        <div class="table-wrap">
          <table class="data-table">
            <thead>
              <tr>
                <th scope="col">Game</th>
                <th scope="col">State</th>
                <th scope="col">Progress</th>
                <th scope="col">Reported</th>
              </tr>
            </thead>
            <tbody>
              <TableState v-if="detail.trees.length === 0" :colspan="4" :rows="detail.trees" empty="None." />
              <tr v-for="t in detail.trees" :key="t.tree_id" class="machine-tree">
                <th scope="row">{{ treeName(t.tree_id) }}</th>
                <td><Badge :tone="t.state === 'ready' ? 'ok' : t.state === 'failed' ? 'bad' : 'warn'">{{ TREE_STATES[t.state] ?? t.state }}</Badge></td>
                <td class="min-w-[12rem]">
                  <div
                    v-if="treeFraction(t) !== null && t.state !== 'ready'"
                    class="mb-1 h-1.5 overflow-hidden rounded-full bg-hover"
                    role="progressbar"
                    :aria-valuenow="Math.round(treeFraction(t)! * 100)"
                    aria-valuemin="0"
                    aria-valuemax="100"
                    :aria-label="`${treeName(t.tree_id)} fetched`"
                  >
                    <div class="h-full bg-accent" :style="{ width: `${treeFraction(t)! * 100}%` }"></div>
                  </div>
                  <span class="break-words" :class="t.state === 'failed' ? 'text-danger' : ''">{{ treeProgress(t) }}</span>
                </td>
                <td class="whitespace-nowrap text-xs text-ink-muted"><RelativeTime v-if="t.reported_at" :value="t.reported_at" :now="now" /></td>
              </tr>
            </tbody>
          </table>
        </div>
      </section>

      <section class="space-y-2" aria-labelledby="machine-facts">
        <h2 id="machine-facts" class="section-heading">Details</h2>
        <dl class="machine-facts card grid gap-x-6 gap-y-1.5 p-4 text-sm sm:grid-cols-[auto_1fr]">
          <dt class="text-ink-muted">Instance</dt>
          <dd class="font-mono text-xs">{{ machine.cloud_id || '—' }}</dd>
          <dt class="text-ink-muted">hostd</dt>
          <dd>{{ machine.agent_version || '—' }}<template v-if="machine.agent_protocol !== null">, protocol {{ machine.agent_protocol }}</template></dd>
          <dt class="text-ink-muted">hostd image</dt>
          <dd class="flex min-w-0 flex-wrap items-center gap-2">
            <span class="break-all font-mono text-xs">{{ machine.agent_image ? shortImage(machine.agent_image) : '—' }}</span>
            <Badge v-if="agentMatch.text" :tone="agentMatch.tone" :title="machine.release.agent_image ? shortImage(machine.release.agent_image) : undefined">{{ agentMatch.text }}</Badge>
          </dd>
          <dt class="text-ink-muted">Machine image</dt>
          <dd class="flex flex-wrap items-center gap-2">
            <span class="font-mono text-xs">{{ machine.image_id || '—' }}</span>
            <Badge v-if="imageMatch.text" :tone="imageMatch.tone" :title="machine.release.image_id ?? undefined">{{ imageMatch.text }}</Badge>
          </dd>
          <dt class="text-ink-muted">Log stream</dt>
          <dd class="break-all font-mono text-xs">{{ machine.agent_log_stream || '—' }}</dd>
          <dt class="text-ink-muted">Address</dt>
          <dd>
            <span class="font-mono text-xs">{{ machine.public_address || '—' }}</span>
            <template v-if="machine.address">
              · static <RouterLink to="/admin/billing/addresses" class="entity-link font-mono text-xs">{{ machine.address.name }}</RouterLink>
            </template>
          </dd>
          <dt class="text-ink-muted">Launched</dt>
          <dd>
            <RelativeTime v-if="machine.launched_at" :value="machine.launched_at" :now="now" /><template v-else>—</template>
            <template v-if="machine.ready_at">, ready <RelativeTime :value="machine.ready_at" :now="now" /></template>
          </dd>
          <template v-if="machine.ended_at">
            <dt class="text-ink-muted">Ended</dt>
            <dd><RelativeTime :value="machine.ended_at" :now="now" /><template v-if="machine.end_reason">: {{ machine.end_reason }}</template></dd>
          </template>
        </dl>
      </section>

      <section class="space-y-2" aria-labelledby="machine-timeline">
        <h2 id="machine-timeline" class="section-heading">Timeline</h2>
        <p v-if="detail.events.length === 0" class="text-sm text-ink-muted">None.</p>
        <ol v-else class="card max-h-72 space-y-0.5 overflow-y-auto p-3 text-sm" tabindex="0" aria-labelledby="machine-timeline">
          <li v-for="(e, index) in detail.events" :key="index" class="machine-event flex flex-col gap-x-3 sm:flex-row">
            <RelativeTime :value="e.at" :now="now" class="shrink-0 text-xs text-ink-muted sm:w-28" />
            <span class="break-words">{{ eventText(e) }}</span>
          </li>
        </ol>
      </section>

      <UsageCharts id-base="machine-usage" subject="machine" :id="machine.id" :name="`Machine ${machine.id}`" :heading-level="2" :follow="live" />

      <LogViewer id-base="machine-log" subject="machine" :id="machine.id" heading="hostd log" :heading-level="2" :followable="live" />
    </template>
  </div>
</template>

<script setup lang="ts">
// /admin/machines/<id>: one fleet machine (GET /api/admin/fleet/machines/<id>):
// its state and last report, what is wrong, what it uses, its slots with the
// servers in them (joined to the fleet's rows for their game, players and
// owner), the games it holds, its facts against the release, its timeline,
// usage and hostd log; drain and end while it lives. Polled every 10 s while
// it lives.
import { computed, ref, watch } from 'vue';
import { RouterLink, useRoute } from 'vue-router';
import LogViewer from '../../../components/LogViewer.vue';
import RelativeTime from '../../../components/RelativeTime.vue';
import UsageCharts from '../../../components/UsageCharts.vue';
import StatTile from '../../../components/charts/StatTile.vue';
import PlayerLink from '../../../components/links/PlayerLink.vue';
import ServerLink from '../../../components/links/ServerLink.vue';
import Badge from '../../../components/ui/Badge.vue';
import LiveNotices from '../../../components/ui/LiveNotices.vue';
import Notice from '../../../components/ui/Notice.vue';
import PageHeader from '../../../components/ui/PageHeader.vue';
import TableState from '../../../components/ui/TableState.vue';
import { fetchHostedFleet, fetchMachine, machineAction } from '../../../api/admin';
import { fetchProducts } from '../../../api/products';
import { confirmAction } from '../../../composables/useConfirm';
import { usePolling } from '../../../composables/usePolling';
import type { FleetServer, MachineDetail } from '../../../types/hostedServer';
import type { ProductsResponse } from '../../../types/products';
import type { Crumb } from '../../../utils/adminNav';
import { machineKindLabel, machineStateLabel } from '../../../utils/adminView';
import { formatBytes } from '../../../utils/format';
import { gameName } from '../../../utils/games';
import { stateLabel } from '../../../utils/hostedState';
import { shortImage } from '../../../utils/infraView';
import {
  eventText,
  machineFigures,
  machineFlags,
  machineLive,
  machineTone,
  releaseMatch,
  reportAge,
  treeFraction,
  treeProgress,
} from '../../../utils/machineView';
import { serverStateTone } from '../../../utils/tone';

const POLL_MS = 10_000;
const CRUMBS: Crumb[] = [
  { label: 'Servers', to: '/admin/servers' },
  { label: 'Machines', to: '/admin/servers/machines' },
];
const TREE_STATES: Record<string, string> = { fetching: 'Fetching', ready: 'Ready', failed: 'Failed' };

const route = useRoute();
const id = computed(() => String(route.params.id ?? ''));

const detail = ref<MachineDetail | null>(null);
const servers = ref<FleetServer[]>([]);
const products = ref<ProductsResponse | null>(null);
const missing = ref(false);
const busy = ref(false);
const message = ref('');
const error = ref('');
const pollTrouble = ref(false);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
// The error shown is a failed read's (not an action's refusal).
let errorFromLoad = false;

const machine = computed(() => detail.value?.machine ?? null);
const live = computed(() => (machine.value ? machineLive(machine.value.state) : false));
const report = computed(() => (machine.value ? reportAge(machine.value, now.value) : null));
const flags = computed(() => (machine.value ? machineFlags(machine.value, now.value) : []));
const canDrain = computed(() => machine.value?.state === 'ready' || machine.value?.state === 'booting');
const canEnd = computed(() => !!machine.value && !['ended', 'ending', 'failed'].includes(machine.value.state));

// CPU, memory, disk and network as tiles, flagged past the health check's
// thresholds.
const tiles = computed(() =>
  machineFigures(machine.value?.stats ?? null).map((f) => ({ label: f.label, value: f.value, detail: f.flag === 'low' ? 'Low' : f.flag ? 'High' : undefined, flag: f.flag })),
);

const agentMatch = computed(() => releaseMatch(machine.value?.agent_image || null, machine.value?.release.agent_image ?? null));
const imageMatch = computed(() => releaseMatch(machine.value?.image_id ?? null, machine.value?.release.image_id ?? null));

// Its slots that hold a server, by slot, joined to the fleet's row of each
// server for its game, players and owner.
const slotRows = computed(() =>
  [...(machine.value?.slot_holders ?? [])]
    .sort((a, b) => a.slot - b.slot)
    .map((h) => {
      const s = servers.value.find((x) => x.id === h.server_id) ?? null;
      const max = s?.listing?.max_players ?? s?.settings.max_players;
      return {
        slot: h.slot,
        serverId: h.server_id,
        state: h.state,
        name: h.name || s?.listing?.server_name || s?.settings.game_name || `Server ${h.server_id}`,
        game: s ? gameName(s) : '',
        players: s?.players != null ? `${s.players}/${max}` : '',
        owner: s?.owner ?? null,
        late: h.tick?.late ?? s?.tick?.late ?? null,
      };
    }),
);

// A tree by the game it is: a server's pinned base or expansion, else a
// product's current tree; "Tree N" when none matches.
const treeNames = computed(() => {
  const names = new Map<number, string>();
  for (const p of products.value?.products ?? []) {
    if (p.current_tree) names.set(p.current_tree.id, `${p.display_name} ${p.current_tree.version}`.trim());
  }
  for (const s of servers.value) {
    if (s.data.base) names.set(s.data.base.tree_id, `${gameName(s)} ${s.data.base.version}`.trim());
    if (s.data.expansion) names.set(s.data.expansion.tree_id, `${s.expansion?.display_name || s.expansion_display_name || 'Expansion'} ${s.data.expansion.version}`.trim());
  }
  return names;
});
function treeName(treeId: number): string {
  return treeNames.value.get(treeId) ?? `Tree ${treeId}`;
}

const poll = usePolling({
  async read(signal, background) {
    if (!/^\d+$/.test(id.value)) {
      missing.value = true;
      error.value = 'No machine has this id.';
      return false;
    }
    const [result, fleet, prods] = await Promise.all([
      fetchMachine(id.value, signal),
      fetchHostedFleet({ state: 'live' }, signal),
      // The games change rarely: read once.
      products.value ? Promise.resolve(null) : fetchProducts(signal),
    ]);
    if (signal.aborted) return false;
    now.value = Date.now();
    // The servers' and games' names are niceties: a failure says nothing.
    if (fleet.ok) servers.value = fleet.value.servers;
    if (prods && prods.ok) products.value = prods.value;
    if (result.ok) {
      detail.value = result.value;
      loadedAt.value = now.value;
      pollTrouble.value = false;
      if (errorFromLoad) error.value = '';
      errorFromLoad = false;
      return true;
    }
    if (result.error.code === 'not_found') {
      missing.value = true;
      detail.value = null;
      error.value = 'No machine has this id.';
      return false;
    }
    if (background && detail.value !== null) {
      pollTrouble.value = true;
    } else {
      error.value = result.error.message;
      errorFromLoad = true;
    }
    return false;
  },
  next: () => (missing.value || !live.value ? null : POLL_MS),
});

async function onAction(action: 'drain' | 'end') {
  const m = machine.value;
  if (!m || busy.value) return;
  const n = m.slots_used;
  const servers = `${n} server${n === 1 ? '' : 's'}`;
  const prompt =
    action === 'drain'
      ? `Drain machine ${m.id}? Its ${servers} move to other machines one at a time.`
      : n === 0
        ? `End machine ${m.id}? No server runs on it.`
        : `End machine ${m.id} now? Its ${servers} move at once and their matches end.`;
  if (!(await confirmAction(prompt))) return;
  busy.value = true;
  const result = await machineAction(m.id, action);
  busy.value = false;
  if (!result.ok) {
    error.value = result.error.message;
    message.value = '';
    errorFromLoad = false;
  } else {
    message.value = `Machine ${m.id} ${action === 'drain' ? 'draining' : 'ending'}.`;
    error.value = '';
  }
  await poll.readAgain();
}

// Another machine's page in the same place: start over.
watch(id, () => {
  detail.value = null;
  missing.value = false;
  error.value = '';
  message.value = '';
  void poll.refresh();
});
</script>
