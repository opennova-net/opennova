<template>
  <div class="space-y-6">
    <PageHeader
      :title="plainTitle"
      :crumbs="crumbs"
      :tabs="server ? tabs : []"
      refreshable
      :loaded-at="loadedAt"
      :now="now"
      :loading="poll.loading.value"
      @refresh="poll.refresh"
    >
      <template v-if="server" #title><GameText :text="titleText" /></template>
      <template v-if="server" #meta>
        <Badge :tone="serverStateTone(server.state)">{{ scope === 'admin' ? stateLabel(server.state) : ownerStateLabel(server.state) }}</Badge>
        <ProductLink v-if="scope === 'admin' && server.product" :product-ref="server.product.key || server.product.id" :name="gameName(server)" />
        <span v-else>{{ gameName(server) }}</span>
        <span v-if="server.expansion || server.expansion_display_name">{{ server.expansion?.display_name || server.expansion_display_name }}</span>
        <span v-if="gameType" class="server-game-type"><GameText :text="gameType" /></span>
        <span v-if="server.listing?.mission_name" class="server-map"><GameText :text="server.listing.mission_name" /></span>
        <span v-if="server.listing" class="server-players">{{ server.listing.players }}/{{ server.listing.max_players }} players</span>
        <template v-if="fleet">
          <PlayerLink v-if="fleet.owner" :id="fleet.owner.id" :name="fleet.owner.username" />
          <PlayerLink v-else name="" />
          <span v-if="fleet.machine_ref"><MachineLink :id="fleet.machine_ref" /> slot {{ fleet.slot ?? '—' }}</span>
        </template>
        <span v-if="server.address" class="server-address font-mono text-xs" :title="`Static address ${server.address.name}`">
          {{ server.address.hostname || server.address.name }}<template v-if="serverAddressEndpoint(server.address)"> {{ serverAddressEndpoint(server.address) }}</template>
        </span>
      </template>
      <template v-if="server" #actions>
        <RouterLink v-if="scope === 'owner'" :to="{ path: '/me/servers/new', query: { from: String(server.id) } }" class="btn-secondary">
          Start again with these
        </RouterLink>
        <button v-if="server.live && server.state !== 'stopping'" type="button" class="btn-danger" :disabled="stopping" @click="stop">
          {{ stopping ? 'Stopping…' : 'Stop…' }}
        </button>
      </template>
    </PageHeader>

    <LiveNotices :message="message" :error="error" :poll-trouble="pollTrouble" />
    <p v-if="server === null && !error" class="text-sm text-ink-muted">Loading…</p>

    <RouterView v-if="server" v-slot="{ Component }">
      <component :is="Component" :server="server" :scope="scope" :now="now" @changed="onChanged" @gone="poll.readAgain" @session-ended="onSessionEnded" />
    </RouterView>
  </div>
</template>

<script setup lang="ts">
// One hosted server's page, the owner's (/me/servers/<id>: GET
// /api/me/servers/<id>) and the admin's (/admin/servers/<id>: GET
// /api/admin/hosted/<id>): its name, state, game, map, players (and for the
// admin its owner and machine) over its tabs, each a child route handed the
// server; Start again (the owner's) and Stop. Polls on the hosted cadence and
// provides the console queue its Console tab's calls go through.
import { computed, ref, watch } from 'vue';
import { RouterLink, RouterView, useRoute, useRouter } from 'vue-router';
import GameText from '../../components/GameText.vue';
import MachineLink from '../../components/links/MachineLink.vue';
import PlayerLink from '../../components/links/PlayerLink.vue';
import ProductLink from '../../components/links/ProductLink.vue';
import Badge from '../../components/ui/Badge.vue';
import LiveNotices from '../../components/ui/LiveNotices.vue';
import PageHeader from '../../components/ui/PageHeader.vue';
import type { Tab } from '../../components/ui/TabNav.vue';
import { fetchFleetServer, stopFleetServer } from '../../api/admin';
import { fetchMyServer, stopHostedServer } from '../../api/hostedServer';
import { confirmAction } from '../../composables/useConfirm';
import { provideConsoleQueue } from '../../composables/useConsoleQueue';
import { hostedCadence, usePolling } from '../../composables/usePolling';
import { useSession } from '../../composables/useSession';
import type { FleetServer, HostedServer } from '../../types/hostedServer';
import { serverAddressEndpoint } from '../../utils/addressView';
import type { Crumb } from '../../utils/adminNav';
import { gameName, gameTypeName, serverName } from '../../utils/games';
import { stripGameText } from '../../utils/gameText';
import { ownerStateLabel, stateLabel } from '../../utils/hostedState';
import { serverStateTone } from '../../utils/tone';

const props = defineProps<{
  scope: 'owner' | 'admin';
}>();

const route = useRoute();
const router = useRouter();
const { refresh: refreshSession, isAdmin } = useSession();
provideConsoleQueue();

const server = ref<HostedServer | null>(null);
const missing = ref(false);
const stopping = ref(false);
const message = ref('');
const error = ref('');
const pollTrouble = ref(false);
const loadedAt = ref<number | null>(null);
const now = ref(Date.now());
// The error shown is a failed read's (not the stop's refusal).
let errorFromLoad = false;

const id = computed(() => Number(route.params.id));
const base = computed(() => (props.scope === 'admin' ? `/admin/servers/${id.value}` : `/me/servers/${id.value}`));
const crumbs = computed<Crumb[]>(() => [{ label: 'Servers', to: props.scope === 'admin' ? '/admin/servers' : '/me/servers' }]);
// The admin's page reads a FleetServer: its owner and machine.
const fleet = computed(() => (props.scope === 'admin' && server.value ? (server.value as FleetServer) : null));

const titleText = computed(() => serverName(server.value?.listing?.server_name || server.value?.settings.game_name));
const plainTitle = computed(() => (server.value ? stripGameText(titleText.value) : `Server ${route.params.id ?? ''}`));
const gameType = computed(() => gameTypeName(server.value?.listing?.game_type));

// The owner's tabs; an admin's add Logs, on the owner's page too (an owner
// who is no admin never sees raw logs: GET /api/admin/logs).
const tabs = computed<Tab[]>(() => {
  const common = [
    { label: 'Overview', to: base.value },
    { label: 'Stats', to: `${base.value}/stats` },
    { label: 'Console', to: `${base.value}/console` },
  ];
  return props.scope === 'admin' || isAdmin.value ? [...common, { label: 'Logs', to: `${base.value}/logs` }] : common;
});

async function onSessionEnded() {
  await refreshSession();
  // The admin gate closes on its own once the session is re-read.
  if (props.scope === 'owner') await router.replace({ path: '/login', query: { redirect: route.fullPath } });
}

const cadence = hostedCadence();

const poll = usePolling({
  async read(signal, background) {
    if (!Number.isInteger(id.value) || id.value <= 0) {
      missing.value = true;
      error.value = 'No server has this id.';
      return false;
    }
    const result = props.scope === 'admin' ? await fetchFleetServer(id.value, signal) : await fetchMyServer(id.value, signal);
    if (signal.aborted) return false;
    now.value = Date.now();
    if (result.ok) {
      // A reply without the timeline (a stop's) keeps the one shown.
      const events = result.value.events ?? server.value?.events;
      server.value = { ...result.value, events };
      loadedAt.value = now.value;
      pollTrouble.value = false;
      if (errorFromLoad) error.value = '';
      errorFromLoad = false;
      return true;
    }
    if (result.error.code === 'unauthorized') {
      await onSessionEnded();
      return false;
    }
    if (result.error.code === 'not_found') {
      missing.value = true;
      server.value = null;
      error.value = props.scope === 'admin' ? 'No server has this id.' : 'Not one of your servers, or it no longer exists.';
      return false;
    }
    if (background && server.value !== null) {
      pollTrouble.value = true;
    } else {
      error.value = result.error.message;
      errorFromLoad = true;
    }
    return false;
  },
  next: () => (missing.value ? null : cadence(server.value ? [server.value] : [])),
});

// Another server's page in the same layout: start over.
watch(id, () => {
  server.value = null;
  missing.value = false;
  message.value = '';
  error.value = '';
  pollTrouble.value = false;
  void poll.refresh();
});

// A tab changed the server (its update went in): show the reply at once, and
// follow it from there.
function onChanged(updated: HostedServer | null) {
  if (updated && server.value && updated.id === server.value.id) {
    server.value = { ...server.value, ...updated, events: updated.events ?? server.value.events };
  }
  void poll.readAgain();
}

async function stop() {
  const s = server.value;
  if (!s || stopping.value) return;
  const name = plainTitle.value;
  if (!(await confirmAction(`Stop ${name}? Its match ends for everyone on it.`))) return;
  stopping.value = true;
  const result = props.scope === 'admin' ? await stopFleetServer(s.id) : await stopHostedServer(s.id);
  stopping.value = false;
  if (!result.ok) {
    error.value = result.error.message;
    message.value = '';
    errorFromLoad = false;
    if (result.error.code === 'unauthorized') await onSessionEnded();
    else if (result.error.code === 'not_found' || result.error.code === 'not_live') void poll.readAgain();
    return;
  }
  message.value = result.value?.state === 'stopped' ? `${name} has stopped.` : `${name} is stopping.`;
  error.value = '';
  onChanged(result.value);
}
</script>
