<template>
  <section class="space-y-4" :aria-labelledby="`${idBase}-heading`">
    <div class="space-y-1">
      <div class="flex flex-col gap-1 sm:flex-row sm:items-baseline sm:justify-between">
        <h2 :id="`${idBase}-heading`" class="text-xl font-semibold text-ink">Lobbies</h2>
        <p v-if="loadedAt" class="text-xs text-ink-muted">Read <RelativeTime :value="loadedAt" :now="now" />, again every 15 seconds</p>
      </div>
      <p class="text-sm text-ink-muted">
        The lobby instances that hold the sessions above, one per lobby task, each linked to its task's usage. A draining
        one is handed no new client by the gates; the sessions it holds stay until they leave.
      </p>
    </div>

    <!-- Live regions, always in the page so a screen reader hears what lands in
         them, each its own box while it holds something. -->
    <div role="status" :class="message ? 'rounded-panel border border-online/40 bg-online/10 p-4 text-sm text-online' : 'sr-only'">{{ message }}</div>
    <div role="alert" :class="error ? 'rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger' : 'sr-only'">{{ error }}</div>

    <div class="relative overflow-x-auto rounded-panel border border-border bg-panel">
      <table class="min-w-full divide-y divide-border">
        <caption class="sr-only">Lobby instances</caption>
        <thead>
          <tr class="bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted">
            <th scope="col" class="px-4 py-3">Lobby</th>
            <th scope="col" class="px-4 py-3">State</th>
            <th scope="col" class="px-4 py-3">Sessions</th>
            <th scope="col" class="px-4 py-3">Task</th>
            <th scope="col" class="px-4 py-3">Started</th>
            <th scope="col" class="px-4 py-3">Last seen</th>
            <th scope="col" class="px-4 py-3 text-right">Actions</th>
          </tr>
        </thead>
        <tbody class="divide-y divide-border text-sm text-ink">
          <tr v-if="data === null">
            <td colspan="7" class="px-4 py-6 text-center text-ink-muted">{{ poll.loading.value ? 'Loading…' : 'Not loaded.' }}</td>
          </tr>
          <tr v-else-if="data.lobbies.length === 0">
            <td colspan="7" class="px-4 py-6 text-center text-ink-muted">No lobby instance is registered.</td>
          </tr>
          <tr v-for="l in data?.lobbies ?? []" :key="l.id" class="lobby-row hover:bg-hover">
            <th scope="row" class="whitespace-nowrap px-4 py-3 text-left font-normal">
              Lobby {{ l.id }}
              <span class="block font-mono text-xs text-ink-muted">{{ l.endpoint }}</span>
            </th>
            <td class="whitespace-nowrap px-4 py-3">
              <span :class="toneBadgeClass(lobbyStateLabel(l, now).tone)">{{ lobbyStateLabel(l, now).label }}</span>
            </td>
            <td class="whitespace-nowrap px-4 py-3">{{ countLabel(l.sessions, 'session') }}</td>
            <td class="whitespace-nowrap px-4 py-3">
              <RouterLink
                :to="lobbyUsageLink(l.task_id)"
                class="text-accent hover:text-ink-bright"
                :class="{ 'font-mono text-xs': l.task_id !== '' }"
                :title="l.task_id ? `Task ${l.task_id}` : undefined"
              >
                {{ l.task_id ? shortTaskId(l.task_id) : 'Its usage' }}
              </RouterLink>
            </td>
            <td class="whitespace-nowrap px-4 py-3 text-xs text-ink-muted">
              <RelativeTime v-if="sessionTime(l.started_ms, now) !== null" :value="l.started_ms" :now="now" />
              <template v-else>Unknown</template>
            </td>
            <td class="whitespace-nowrap px-4 py-3 text-xs text-ink-muted">
              <RelativeTime v-if="sessionTime(l.last_seen_ms, now) !== null" :value="l.last_seen_ms" :now="now" />
              <template v-else>Unknown</template>
            </td>
            <td class="whitespace-nowrap px-4 py-3 text-right">
              <button
                v-if="l.state === 'active'"
                type="button"
                :disabled="busy"
                class="text-danger/80 hover:text-danger disabled:opacity-40"
                @click="drain(l)"
              >
                Drain<span class="sr-only"> lobby {{ l.id }}</span>
              </button>
              <span v-else class="text-xs text-ink-muted">Draining</span>
            </td>
          </tr>
        </tbody>
      </table>
    </div>
  </section>
</template>

<script setup lang="ts">
// The lobby instances (GET /api/admin/lobbies), on the NovaWorld page: each
// lobby's address, whether it takes clients (taking clients, draining, or not
// seen for 15 s), its sessions, the task it runs as (linked to that task's
// usage on the lobby program's page), when it started and was last seen.
// Drain (POST /api/admin/lobbies/<id>/drain, once confirmed) has the gates hand
// it no new client; its sessions stay. Polled every 15 s; refresh() reads it
// again (the page's Refresh).
import { ref } from 'vue';
import { RouterLink } from 'vue-router';
import RelativeTime from './RelativeTime.vue';
import { drainLobby, fetchLobbies } from '../api/admin';
import { usePolling } from '../composables/usePolling';
import type { AdminLobby, LobbiesResponse } from '../types/admin';
import { lobbyStateLabel, lobbyUsageLink, sessionTime, shortTaskId } from '../utils/adminView';
import { countLabel, toneBadgeClass } from '../utils/infraView';

withDefaults(defineProps<{ idBase?: string }>(), { idBase: 'lobbies' });

const POLL_MS = 15_000;

const data = ref<LobbiesResponse | null>(null);
const error = ref('');
const message = ref('');
const busy = ref(false);
const now = ref(Date.now());
const loadedAt = ref<number | null>(null);
// The error banner shows a failed read (not an action's refusal).
let errorFromLoad = false;

async function drain(l: AdminLobby) {
  if (busy.value) return;
  if (
    !window.confirm(
      `Drain lobby ${l.id}? The gates hand it no new client from their next read; the ${countLabel(l.sessions, 'session')} it holds stay until they leave, and its task stays in the load balancer.`,
    )
  )
    return;
  busy.value = true;
  const result = await drainLobby(l.id);
  busy.value = false;
  if (!result.ok) {
    error.value = result.error.message;
    message.value = '';
    errorFromLoad = false;
  } else {
    message.value = `Lobby ${l.id} is draining.`;
    error.value = '';
  }
  await poll.readAgain();
}

const poll = usePolling({
  async read(signal, background) {
    const result = await fetchLobbies(signal);
    if (signal.aborted) return false;
    now.value = Date.now();
    if (result.ok) {
      data.value = result.value;
      loadedAt.value = now.value;
      if (errorFromLoad) {
        error.value = '';
        errorFromLoad = false;
      }
      return true;
    }
    if (!background || data.value === null) {
      error.value = result.error.message;
      message.value = '';
      errorFromLoad = true;
    }
    return false;
  },
  next: () => POLL_MS,
});

defineExpose({ refresh: poll.refresh, loading: poll.loading });
</script>
