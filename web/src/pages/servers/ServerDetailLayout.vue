<template>
  <div class="space-y-6">
    <RouterLink :to="backPath" class="text-sm text-accent hover:text-ink-bright">{{ backLabel }}</RouterLink>

    <div v-if="server === null && !loadError" class="animate-pulse rounded-panel border border-border bg-panel p-8 text-center text-ink-muted">
      Loading the server…
    </div>

    <div v-else-if="server === null" class="space-y-3 rounded-panel border border-border bg-panel p-6">
      <h1 class="text-xl font-semibold text-ink">{{ missing ? 'No such server' : 'The server could not be loaded' }}</h1>
      <p role="alert" class="text-sm text-ink-muted">{{ loadError }}</p>
      <button
        v-if="!missing"
        type="button"
        class="rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover"
        @click="poll.refresh"
      >
        Try again
      </button>
    </div>

    <template v-else>
      <header class="space-y-3">
        <p v-if="scope === 'admin'" class="text-sm text-ink-muted">
          Hosted server {{ server.id }},
          <template v-if="owner">
            owned by
            <RouterLink :to="`/admin/users/${owner.id}`" class="text-accent hover:text-ink-bright">{{ owner.username }}</RouterLink>
            ({{ owner.tier ? `Tier ${owner.tier}` : 'no tier' }})
          </template>
          <template v-else>whose owner was deleted</template>
        </p>
        <h1
          :id="`hosted-server-${server.id}-heading`"
          ref="heading"
          tabindex="-1"
          class="break-words text-3xl font-semibold text-ink focus:outline-none"
        >
          {{ server.settings.game_name || 'Unnamed server' }}
        </h1>
        <p class="flex flex-wrap items-center gap-2 text-sm text-ink-muted">
          <span :class="stateBadgeClass(server.state)">{{ ownerStateLabel(server.state) }}</span>
          <span>{{ server.game_display_name || server.settings.game }}</span>
          <span v-if="server.settings.expansion">{{ server.expansion_display_name || server.settings.expansion }}</span>
          <span v-if="server.own_machine" class="rounded-control bg-hover px-2 py-0.5 text-xs text-ink">Own machine</span>
        </p>
        <p v-if="pollTrouble" class="text-xs text-warn">The last refresh did not get through; this page keeps trying.</p>
      </header>

      <nav :aria-label="`${server.settings.game_name || 'Server'} pages`" class="border-b border-border">
        <ul class="-mb-px flex gap-1 overflow-x-auto">
          <li v-for="tab in tabs" :key="tab.path">
            <RouterLink
              :to="tab.path"
              class="block whitespace-nowrap border-b-2 px-3 py-2 text-sm font-medium transition"
              :class="isActive(tab) ? 'border-accent text-ink' : 'border-transparent text-ink-muted hover:text-ink'"
              :aria-current="isActive(tab) ? 'page' : undefined"
            >
              {{ tab.label }}
            </RouterLink>
          </li>
        </ul>
      </nav>

      <RouterView v-slot="{ Component }">
        <component
          :is="Component"
          :server="server"
          :scope="scope"
          :now="now"
          @changed="onChanged"
          @gone="poll.readAgain"
          @session-ended="onSessionEnded"
        />
      </RouterView>
    </template>
  </div>
</template>

<script setup lang="ts">
// One hosted server's own page, the owner's (/me/servers/<id>, scope owner:
// GET /api/me/servers/<id>) and the admin's (scope admin: GET
// /api/admin/hosted/<id>): its name and state over tabs, each a child route
// handed the server (Overview, Stats, Console, Settings, and the admin's
// extra ones).
// The page polls the server on the hosted cadence (usePolling) and provides
// the console queue its Console tab's calls go through.
import { computed, ref, watch } from 'vue';
import { RouterLink, RouterView, useRoute, useRouter } from 'vue-router';
import { fetchFleetServer } from '../../api/admin';
import { fetchMyServer } from '../../api/hostedServer';
import { provideConsoleQueue } from '../../composables/useConsoleQueue';
import { hostedCadence, usePolling } from '../../composables/usePolling';
import { useSession } from '../../composables/useSession';
import type { FleetServer, HostedServer } from '../../types/hostedServer';
import { ownerStateLabel, stateBadgeClass } from '../../utils/hostedState';

const props = defineProps<{
  scope: 'owner' | 'admin';
}>();

interface Tab {
  path: string;
  label: string;
}

const route = useRoute();
const router = useRouter();
const { refresh: refreshSession } = useSession();
provideConsoleQueue();

const server = ref<HostedServer | null>(null);
const loadError = ref('');
const missing = ref(false);
const pollTrouble = ref(false);
const now = ref(Date.now());
const heading = ref<HTMLElement | null>(null);

const id = computed(() => Number(route.params.id));
const base = computed(() => (props.scope === 'admin' ? `/admin/fleet/${id.value}` : `/me/servers/${id.value}`));
const backPath = computed(() => (props.scope === 'admin' ? '/admin/fleet' : '/me/servers'));
const backLabel = computed(() => (props.scope === 'admin' ? 'Fleet' : 'All servers'));
const owner = computed(() => (server.value as FleetServer | null)?.owner ?? null);

// The owner's tabs; the admin's name the settings tab for the game's (beside
// the machine's) and add the Machine tab.
const tabs = computed<Tab[]>(() => {
  const common = [
    { path: base.value, label: 'Overview' },
    { path: `${base.value}/stats`, label: 'Stats' },
    { path: `${base.value}/console`, label: 'Console' },
  ];
  if (props.scope === 'admin') {
    return [
      ...common,
      { path: `${base.value}/game-settings`, label: 'Game settings' },
      { path: `${base.value}/machine`, label: 'Machine' },
    ];
  }
  return [...common, { path: `${base.value}/settings`, label: 'Settings' }];
});

function isActive(tab: Tab): boolean {
  return route.path.replace(/\/$/, '') === tab.path;
}

async function onSessionEnded() {
  if (props.scope === 'admin') {
    // The admin gate closes on its own once the session is re-read.
    await refreshSession();
    return;
  }
  await refreshSession();
  await router.replace({ path: '/login', query: { redirect: route.fullPath } });
}

const cadence = hostedCadence();

const poll = usePolling({
  async read(signal, background) {
    if (!Number.isInteger(id.value) || id.value <= 0) {
      missing.value = true;
      loadError.value = 'There is no server at this address.';
      return false;
    }
    const result =
      props.scope === 'admin' ? await fetchFleetServer(id.value, signal) : await fetchMyServer(id.value, signal);
    if (signal.aborted) return false;
    now.value = Date.now();
    if (result.ok) {
      // A reply without the timeline (a stop's) keeps the one shown.
      const events = result.value.events ?? server.value?.events;
      server.value = { ...result.value, events };
      loadError.value = '';
      pollTrouble.value = false;
      return true;
    }
    if (result.error.code === 'unauthorized') {
      await onSessionEnded();
      return false;
    }
    if (result.error.code === 'not_found') {
      missing.value = true;
      server.value = null;
      loadError.value =
        props.scope === 'admin'
          ? 'No hosted server has this id.'
          : 'This server is not one of yours, or it no longer exists.';
      return false;
    }
    if (background && server.value !== null) pollTrouble.value = true;
    else loadError.value = result.error.message;
    return false;
  },
  next: () => (missing.value ? null : cadence(server.value ? [server.value] : [])),
});

// Another server's page in the same layout: start over.
watch(id, () => {
  server.value = null;
  missing.value = false;
  loadError.value = '';
  pollTrouble.value = false;
  void poll.refresh();
});

// A tab changed the server (its stop went in): show the reply at once, and
// follow it from there.
function onChanged(updated: HostedServer | null) {
  if (updated && server.value && updated.id === server.value.id) {
    server.value = { ...server.value, ...updated, events: updated.events ?? server.value.events };
  }
  void poll.readAgain();
}

defineExpose({ focusHeading: () => heading.value?.focus() });
</script>
