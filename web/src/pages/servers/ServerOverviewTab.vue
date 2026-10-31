<template>
  <div class="space-y-6">
    <LiveNotices :message="status" :error="alertText" />

    <section class="space-y-2" :aria-labelledby="ids.progress">
      <h2 :id="ids.progress" class="section-heading">State</h2>
      <ol v-if="stepIndex >= 0" class="flex flex-col gap-2 sm:flex-row sm:gap-1">
        <li
          v-for="(step, index) in OWNER_START_STEPS"
          :key="step.label"
          class="flex items-center gap-2 sm:flex-1 sm:flex-col sm:items-stretch"
          :aria-current="index === stepIndex ? 'step' : undefined"
        >
          <span aria-hidden="true" class="h-2.5 w-2.5 shrink-0 rounded-full sm:h-1.5 sm:w-full" :class="stepBarClass(index)"></span>
          <span class="text-xs" :class="index === stepIndex ? 'font-semibold text-ink' : 'text-ink-muted'">
            {{ step.label }}<span class="sr-only">{{ stepSuffix(index) }}</span>
          </span>
        </li>
      </ol>
      <p v-if="phaseLine" class="server-phase text-sm">{{ phaseLine }}</p>
      <p v-if="scope === 'admin' && server.live && server.phase?.line" class="server-phase-line card break-words p-2 font-mono text-xs">
        <span class="sr-only">The machine's reason: </span>{{ server.phase.line }}
      </p>
      <p v-if="slow" class="server-phase-slow text-sm font-semibold text-warn">{{ SLOW_PHASE_TEXT }}</p>
      <div v-if="!server.live && server.stopped_because" class="server-stopped-because space-y-1 text-sm">
        <p :class="stoppedBad ? 'text-danger' : ''">{{ stoppedBecauseText(server.stopped_because, scope) }}</p>
        <p v-if="server.stopped_because.line" class="card break-words p-2 font-mono text-xs">
          <span class="sr-only">The server's last line: </span>{{ server.stopped_because.line }}
        </p>
      </div>
      <p v-if="server.state === 'failed' && server.failure_reason" class="break-words text-sm text-danger">{{ server.failure_reason }}</p>
      <p v-if="server.live && server.restarts > 0" class="server-restarts text-sm text-warn" title="After a crash or a start that timed out">
        Restarted {{ server.restarts === 1 ? 'once' : `${server.restarts} times` }}
      </p>
      <dl class="server-times flex flex-wrap gap-x-6 gap-y-1 text-sm">
        <div class="flex gap-2"><dt class="text-ink-muted">Requested</dt><dd><RelativeTime :value="server.created_at" :now="now" /></dd></div>
        <div v-if="server.running_at" class="flex gap-2"><dt class="text-ink-muted">First running</dt><dd><RelativeTime :value="server.running_at" :now="now" /></dd></div>
        <div v-if="server.ended_at" class="flex gap-2"><dt class="text-ink-muted">Ended</dt><dd><RelativeTime :value="server.ended_at" :now="now" /></dd></div>
      </dl>
    </section>

    <div class="grid gap-6 lg:grid-cols-2">
      <section class="space-y-2" :aria-labelledby="ids.data">
        <h2 :id="ids.data" class="section-heading">Game</h2>
        <dl class="card grid grid-cols-[auto_1fr] gap-x-6 gap-y-1 p-4 text-sm">
          <dt class="text-ink-muted">{{ gameName(server) }}</dt>
          <dd>{{ versionText(server.data.base) }}</dd>
          <template v-if="server.product?.protocol">
            <dt class="text-ink-muted">Protocol</dt>
            <dd id="hosted-server-protocol">{{ protocolWords(server.product.protocol) }}</dd>
          </template>
          <template v-if="server.expansion || server.data.expansion">
            <dt class="text-ink-muted">{{ expansionName }}</dt>
            <dd>{{ versionText(server.data.expansion) }}</dd>
          </template>
          <template v-if="server.build">
            <dt class="text-ink-muted">Server build</dt>
            <dd id="hosted-server-build"><BuildText :build="server.build" /></dd>
          </template>
        </dl>
        <p v-if="server.update_requested"><Badge tone="info">Update requested</Badge></p>
        <p v-else-if="server.update_available" id="hosted-server-update" class="flex flex-wrap items-center gap-2 text-sm">
          <Badge v-if="contentUpdate" tone="info">{{ updateText }}</Badge>
          <Badge v-if="buildUpdate" tone="info" class="hosted-server-build-update">{{ buildUpdateText }}</Badge>
          <button v-if="scope === 'owner' && server.live && server.state !== 'stopping'" type="button" class="btn-primary btn-sm" :disabled="busy" @click="update">
            {{ busy ? 'Updating…' : 'Update…' }}
          </button>
        </p>
      </section>

      <section v-if="server.live" class="space-y-2" :aria-labelledby="ids.listing">
        <h2 :id="ids.listing" class="section-heading">Listing</h2>
        <dl class="server-listing card grid grid-cols-[auto_1fr] gap-x-6 gap-y-1 p-4 text-sm">
          <dt class="text-ink-muted">NovaWorld</dt>
          <dd>
            <RouterLink v-if="server.listing" :to="`/lobby/${server.listing.rid}`" class="entity-link">In the server browser</RouterLink>
            <span v-else :class="server.state === 'running' ? 'text-warn' : 'text-ink-muted'">Not listed</span>
          </dd>
          <template v-if="server.listing">
            <dt class="text-ink-muted">Address</dt>
            <dd class="break-all font-mono text-xs">{{ server.listing.host_ip }}:{{ server.listing.host_port }}</dd>
          </template>
          <template v-if="server.settings.list_on_novalogic && server.novalogic">
            <dt class="text-ink-muted">NovaLogic</dt>
            <dd class="server-novalogic space-y-0.5">
              <p :class="novalogicClass">{{ novalogicListingText(server.novalogic, now) }}</p>
              <p v-if="server.novalogic.player_slots.length > 0" class="server-novalogic-players text-ink-muted">
                {{ server.novalogic.player_slots.length === 1 ? '1 player' : `${server.novalogic.player_slots.length} players` }} through NovaLogic
              </p>
              <p v-if="scope === 'admin' && server.novalogic.tag" class="font-mono text-xs text-ink-muted">{{ server.novalogic.tag }}</p>
            </dd>
          </template>
        </dl>
      </section>

      <section class="space-y-2" :aria-labelledby="ids.settings">
        <h2 :id="ids.settings" class="section-heading">Settings</h2>
        <dl class="server-settings card grid grid-cols-[auto_1fr] gap-x-6 gap-y-1 p-4 text-sm">
          <dt class="text-ink-muted">{{ FIELD_LABELS.max_players }}</dt>
          <dd>{{ server.settings.max_players }}</dd>
          <dt class="text-ink-muted">{{ FIELD_LABELS.game_password }}</dt>
          <dd>{{ server.settings.game_password ? 'Set' : 'None' }}</dd>
          <dt class="text-ink-muted">{{ FIELD_LABELS.time_limit }}</dt>
          <dd>{{ server.settings.time_limit }} min</dd>
          <dt class="text-ink-muted">{{ FIELD_LABELS.kill_limit }}</dt>
          <dd>{{ limitText(server.settings.kill_limit) }}</dd>
          <dt class="text-ink-muted">{{ FIELD_LABELS.max_score }}</dt>
          <dd>{{ limitText(server.settings.max_score) }}</dd>
          <dt class="text-ink-muted">{{ FIELD_LABELS.respawn }}</dt>
          <dd>{{ server.settings.respawn }} s</dd>
          <dt class="text-ink-muted">{{ FIELD_LABELS.start_delay }}</dt>
          <dd>{{ server.settings.start_delay }} s</dd>
          <dt class="text-ink-muted">{{ FIELD_LABELS.team_ff }}</dt>
          <dd>{{ server.settings.team_ff ? 'On' : 'Off' }}</dd>
          <dt class="text-ink-muted">{{ FIELD_LABELS.send_holdoff_ticks }}</dt>
          <dd>{{ server.settings.send_holdoff_ticks }}<span class="block text-xs text-ink-muted">{{ sendCadenceText(server.settings.send_holdoff_ticks) }}</span></dd>
          <dt class="text-ink-muted">{{ FIELD_LABELS.entity_send_budget }}</dt>
          <dd>{{ server.settings.entity_send_budget }}</dd>
          <dt class="text-ink-muted">{{ FIELD_LABELS.max_packet_size }}</dt>
          <dd>{{ server.settings.max_packet_size }}</dd>
          <dt class="text-ink-muted">{{ FIELD_LABELS.lock_framerate }}</dt>
          <dd>{{ server.settings.lock_framerate ? 'On' : 'Off' }}</dd>
          <template v-if="server.settings.list_on_novalogic">
            <dt class="text-ink-muted">NovaLogic listing</dt>
            <dd>On</dd>
          </template>
          <dt class="text-ink-muted">Rotation</dt>
          <dd class="min-w-0">
            <ol class="server-rotation list-inside list-decimal">
              <li v-for="(mission, index) in server.settings.missions" :key="index" class="break-all">
                <template v-if="titles.get(mission.toLowerCase())">{{ titles.get(mission.toLowerCase()) }}</template>
                <span v-else class="font-mono text-xs">{{ mission }}</span>
              </li>
            </ol>
          </dd>
        </dl>
      </section>

      <section v-if="events.length > 0" class="space-y-2" :aria-labelledby="ids.timeline">
        <h2 :id="ids.timeline" class="section-heading">Timeline</h2>
        <details class="card p-3">
          <summary class="cursor-pointer text-sm text-accent">{{ events.length }} event{{ events.length === 1 ? '' : 's' }}</summary>
          <ol class="mt-2 max-h-80 space-y-1 overflow-y-auto text-sm" tabindex="0" :aria-labelledby="ids.timeline">
            <li v-for="(event, index) in events" :key="index" class="flex flex-col gap-x-3 sm:flex-row">
              <RelativeTime :value="event.at" :now="now" class="shrink-0 text-xs text-ink-muted sm:w-24" />
              <span class="break-words">
                <span class="font-medium">{{ stateLabel(event.to) }}</span><template v-if="event.detail">: {{ event.detail }}</template>
              </span>
            </li>
          </ol>
        </details>
      </section>
    </div>
  </div>
</template>

<script setup lang="ts">
// A hosted server page's Overview: where it is in its life (and why its last
// run ended), the game it runs with the owner's update when a newer release
// waits (POST /api/me/servers/<id>/update), its listing on NovaWorld (and on
// NovaLogic's, when asked), the settings it was started with and its
// timeline. The live controls are the Console tab's; the stop is the page's.
import { computed, onMounted, ref, watch } from 'vue';
import { RouterLink } from 'vue-router';
import BuildText from '../../components/BuildText.vue';
import RelativeTime from '../../components/RelativeTime.vue';
import Badge from '../../components/ui/Badge.vue';
import LiveNotices from '../../components/ui/LiveNotices.vue';
import { fetchProducts } from '../../api/products';
import { fetchMyServers, updateHostedServer } from '../../api/hostedServer';
import { confirmAction } from '../../composables/useConfirm';
import type { HostedEvent, HostedProduct, HostedServer, PinnedTree } from '../../types/hostedServer';
import { gameName, protocolWords, serverName } from '../../utils/games';
import { FIELD_LABELS, sendCadenceText } from '../../utils/hostedSettings';
import {
  OWNER_START_STEPS,
  SLOW_PHASE_TEXT,
  novalogicListingText,
  novalogicListingTone,
  ownerStepIndex,
  phaseSlow,
  phaseText,
  stateLabel,
  stoppedBecauseText,
} from '../../utils/hostedState';
import { stripGameText } from '../../utils/gameText';
import { versionText as buildVersionText, type Build } from '../../utils/buildView';

const props = defineProps<{
  server: HostedServer;
  scope: 'owner' | 'admin';
  // The page's clock (ms).
  now: number;
}>();

const emit = defineEmits<{
  // The update went in: the server as the service answered it.
  (e: 'changed', server: HostedServer | null): void;
  // A refusal said the server is gone or ended: read it again.
  (e: 'gone'): void;
  // A request was refused the session.
  (e: 'session-ended'): void;
}>();

const ids = computed(() => {
  const base = `hosted-server-${props.server.id}`;
  return {
    progress: `${base}-progress`,
    data: `${base}-data`,
    listing: `${base}-listing`,
    settings: `${base}-settings`,
    timeline: `${base}-timeline`,
  };
});

const busy = ref(false);
const status = ref('');
const alertText = ref('');

const events = computed<HostedEvent[]>(() => props.server.events ?? []);
const expansionName = computed(() => props.server.expansion?.display_name || props.server.expansion_display_name || 'Expansion');

function versionText(tree: PinnedTree | null): string {
  if (!tree) return 'Not placed yet';
  return tree.version || 'No version';
}

// KillLimit and MaxScore at 500 are the host screen's "no limit".
function limitText(value: number): string {
  return value >= 500 ? 'No limit' : String(value);
}

// The games the owner may start (GET /api/me/servers), read once: their
// missions' titles, and their current versions while an update waits; the
// admin's versions come from GET /api/admin/products when one waits.
const offered = ref<HostedProduct[] | null>(null);
const adminVersions = ref<Map<number, string> | null>(null);
onMounted(async () => {
  if (props.scope !== 'owner') return;
  const result = await fetchMyServers();
  if (result.ok) offered.value = result.value.products;
});
watch(
  () => props.server.update_available,
  async (waiting) => {
    if (props.scope !== 'admin' || !waiting || adminVersions.value) return;
    const result = await fetchProducts();
    if (result.ok) adminVersions.value = new Map(result.value.products.map((p) => [p.id, p.current_tree?.version ?? '']));
  },
  { immediate: true },
);

const current = computed(() => {
  const productId = props.server.product?.id;
  const expansionId = props.server.expansion?.id;
  if (props.scope === 'owner') {
    const product = offered.value?.find((d) => d.id === productId);
    return product ? { base: product.version, expansion: product.expansions.find((e) => e.id === expansionId)?.version ?? '' } : null;
  }
  const versions = adminVersions.value;
  return versions ? { base: versions.get(productId ?? -1) ?? '', expansion: versions.get(expansionId ?? -1) ?? '' } : null;
});

// What the waiting update brings: newer data, a newer server build, or both (a
// reply that names neither reads as data, the update's first kind).
const contentUpdate = computed(() => props.server.update_kinds.includes('content') || props.server.update_kinds.length === 0);
const buildUpdate = computed(() => props.server.update_kinds.includes('server_build'));

// A build in a few words: ONE's version, else the commit.
function buildWords(build: Build): string {
  return buildVersionText(build) ?? build.commit ?? '';
}

// "New server build: ONE 0.0.22.5 → ONE 0.0.22.9" when both builds are known
// (the commits where the versions read alike), "New server build: ONE 0.0.22.9"
// with only the new one, else "New server build available".
const buildUpdateText = computed(() => {
  const to = props.server.build_available;
  if (!to || !buildWords(to)) return 'New server build available';
  const from = props.server.build;
  if (!from || !buildWords(from)) return `New server build: ${buildWords(to)}`;
  const alike = buildWords(from) === buildWords(to) && from.commit && to.commit;
  return alike
    ? `New server build: ${buildWords(from)} ${from.commit} → ${buildWords(to)} ${to.commit}`
    : `New server build: ${buildWords(from)} → ${buildWords(to)}`;
});

// "Joint Operations 1.0.9 available" (and the expansion's, when it is the
// newer one); without the versions, "Update available".
const updateText = computed(() => {
  const now = current.value;
  const newer: string[] = [];
  if (now) {
    if (now.base && now.base !== (props.server.data.base?.version ?? '')) newer.push(`${gameName(props.server)} ${now.base}`);
    if (now.expansion && now.expansion !== (props.server.data.expansion?.version ?? '')) newer.push(`${expansionName.value} ${now.expansion}`);
  }
  return newer.length === 0 ? 'Update available' : `${newer.join(' and ')} available`;
});

// Each mission's title, as the games the owner may start name them (the
// admin's view lists the files).
const titles = computed(() => {
  const named = new Map<string, string>();
  for (const game of offered.value ?? []) {
    for (const m of [...game.missions, ...game.expansions.flatMap((e) => e.missions)]) {
      if (m.title) named.set(m.file.toLowerCase(), m.title);
    }
  }
  return named;
});

const stepIndex = computed(() => ownerStepIndex(props.server.state));

const novalogicClass = computed(() => {
  const listing = props.server.novalogic;
  if (!listing) return '';
  const tone = novalogicListingTone(listing);
  return tone === 'good' ? 'text-online' : tone === 'bad' ? 'text-danger' : 'text-warn';
});

const phaseLine = computed(() => (props.server.live && props.server.phase ? phaseText(props.server.phase) : ''));
const slow = computed(() => props.server.live && phaseSlow(props.server, props.now));
// An end the server or its limits caused, rather than a stop or an update.
const stoppedBad = computed(() => {
  const cause = props.server.stopped_because?.cause;
  return cause === 'crash' || cause === 'timeout' || cause === 'no_data' || cause === 'link_lost';
});

function stepBarClass(index: number): string {
  if (index < stepIndex.value) return 'bg-online';
  if (index > stepIndex.value) return 'bg-hover';
  return props.server.state === 'running' ? 'bg-online' : 'bg-warn animate-pulse';
}

function stepSuffix(index: number): string {
  if (index < stepIndex.value) return ', done';
  if (index > stepIndex.value) return ', to come';
  return ', current';
}

// The owner's update: the service restarts the server on its game's current
// release (its players drop); the reply shows at once.
async function update() {
  if (busy.value) return;
  const name = stripGameText(serverName(props.server.listing?.server_name || props.server.settings.game_name));
  if (!(await confirmAction(`Update ${name}? It restarts and its players drop.`))) return;
  busy.value = true;
  status.value = '';
  alertText.value = '';
  const result = await updateHostedServer(props.server.id);
  busy.value = false;
  if (!result.ok) {
    alertText.value = result.error.message;
    if (result.error.code === 'unauthorized') emit('session-ended');
    else if (result.error.code === 'not_found' || result.error.code === 'not_live' || result.error.code === 'up_to_date') emit('gone');
    return;
  }
  status.value = `${name} is updating.`;
  emit('changed', result.value);
}
</script>
