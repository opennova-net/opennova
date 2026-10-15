<template>
  <div class="space-y-10">
    <header class="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
      <div>
        <h1 class="text-3xl font-semibold text-ink">NovaWorld sessions</h1>
        <p class="text-sm text-ink-muted">
          Game clients signed in to NovaWorld.
          <span v-if="data">{{ summary }}</span>
        </p>
      </div>
      <button type="button" :class="secondaryButton" :disabled="poll.loading.value" @click="poll.refresh">
        {{ poll.loading.value ? 'Refreshing…' : 'Refresh' }}
      </button>
    </header>

    <p v-if="error" role="alert" class="rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger">{{ error }}</p>

    <section class="space-y-4" aria-labelledby="sessions-heading">
      <h2 id="sessions-heading" class="sr-only">Sessions</h2>
      <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
        <table class="min-w-full divide-y divide-border">
          <caption class="sr-only">NovaWorld sessions, as the service holds them now</caption>
          <thead>
            <tr class="bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted">
              <th scope="col" class="px-4 py-3">Session</th>
              <th scope="col" class="px-4 py-3">State</th>
              <th scope="col" class="px-4 py-3">Game</th>
              <th scope="col" class="px-4 py-3">Account</th>
              <th scope="col" class="px-4 py-3">Address</th>
              <th scope="col" class="px-4 py-3">Connected</th>
              <th scope="col" class="px-4 py-3">Last heard</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border text-sm text-ink">
            <tr v-if="data === null">
              <td colspan="7" class="px-4 py-6 text-center text-ink-muted">{{ poll.loading.value ? 'Loading…' : 'Not loaded.' }}</td>
            </tr>
            <tr v-else-if="data.connections.length === 0">
              <td colspan="7" class="px-4 py-6 text-center text-ink-muted">No game client is signed in.</td>
            </tr>
            <tr v-for="c in data?.connections ?? []" :key="c.id" class="session-row hover:bg-hover">
              <th scope="row" class="px-4 py-3 text-left font-mono text-xs font-normal">{{ c.id }}</th>
              <td class="whitespace-nowrap px-4 py-3">{{ sessionStateLabel(c.state) }}</td>
              <td class="whitespace-nowrap px-4 py-3">
                <span v-if="protocolTitle(c.pn)" :title="`Protocol name ${c.pn}`">{{ protocolTitle(c.pn) }}</span>
                <span v-else class="font-mono text-xs" title="A protocol name this site does not know">{{ c.pn || 'Not said' }}</span>
              </td>
              <td class="px-4 py-3">
                <template v-if="c.identity">{{ c.identity }}</template>
                <span v-else class="text-ink-muted">Not signed in yet</span>
              </td>
              <td class="px-4 py-3 font-mono text-xs">{{ c.addr }}</td>
              <td class="whitespace-nowrap px-4 py-3 text-xs text-ink-muted">
                <RelativeTime v-if="c.created_ms" :value="c.created_ms" :now="now" />
                <template v-else>Unknown</template>
              </td>
              <td class="whitespace-nowrap px-4 py-3 text-xs text-ink-muted">
                <RelativeTime v-if="c.last_seen_ms" :value="c.last_seen_ms" :now="now" />
                <template v-else>Unknown</template>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </section>
  </div>
</template>

<script setup lang="ts">
// /admin/sessions: the NovaWorld sessions the service holds now (GET
// /api/admin/connections): each game client signed in, its state, the game
// its protocol name says, its account, its address, and when it connected and
// was last heard from (relative, with the UTC time on hover). Polled every
// 15 s.
import { computed, ref } from 'vue';
import RelativeTime from '../../components/RelativeTime.vue';
import { fetchConnections } from '../../api/admin';
import { usePolling } from '../../composables/usePolling';
import type { ConnectionsResponse } from '../../types/admin';
import { protocolTitle, sessionStateLabel } from '../../utils/adminView';
import { formatDuration } from '../../utils/format';

const POLL_MS = 15_000;

const secondaryButton =
  'rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40';

const data = ref<ConnectionsResponse | null>(null);
const error = ref('');
const now = ref(Date.now());

// "1 signed in; a session closes after 4 minutes without a keepalive."
const summary = computed(() => {
  const d = data.value;
  if (!d) return '';
  const signedIn = `${d.count} signed in`;
  if (!d.heartbeat_timeout_ms) return `${signedIn}.`;
  return `${signedIn}; a session closes after ${formatDuration(d.heartbeat_timeout_ms)} without a keepalive.`;
});

const poll = usePolling({
  async read(signal, background) {
    const result = await fetchConnections(signal);
    if (signal.aborted) return false;
    now.value = Date.now();
    if (result.ok) {
      data.value = result.value;
      error.value = '';
      return true;
    }
    if (!background || data.value === null) error.value = result.error.message;
    return false;
  },
  next: () => POLL_MS,
});
</script>
