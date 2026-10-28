<template>
  <section class="space-y-3" :aria-labelledby="`${idBase}-heading`">
    <h2 :id="`${idBase}-heading`" class="section-heading">Lobbies</h2>

    <LiveNotices :message="message" :error="error" />

    <div class="table-wrap">
      <table class="data-table">
        <caption class="sr-only">Lobby instances</caption>
        <thead>
          <tr>
            <th scope="col">Lobby</th>
            <th scope="col">State</th>
            <th scope="col">Sessions</th>
            <th scope="col">Task</th>
            <th scope="col">Started</th>
            <th scope="col">Last seen</th>
            <th scope="col"><span class="sr-only">Actions</span></th>
          </tr>
        </thead>
        <tbody>
          <TableState v-if="!data?.lobbies.length" :colspan="7" :rows="data?.lobbies ?? null" :loading="poll.loading.value" empty="No lobby instances." />
          <tr v-for="l in data?.lobbies ?? []" :key="l.id" class="lobby-row">
            <th scope="row" class="whitespace-nowrap font-normal">
              Lobby {{ l.id }}
              <span class="block font-mono text-xs text-ink-muted">{{ l.endpoint }}</span>
            </th>
            <td class="whitespace-nowrap"><Badge :tone="lobbyStateLabel(l, now).tone">{{ lobbyStateLabel(l, now).label }}</Badge></td>
            <td class="whitespace-nowrap">{{ countLabel(l.sessions, 'session') }}</td>
            <td class="whitespace-nowrap">
              <RouterLink :to="lobbyUsageLink(l.task_id)" class="entity-link" :class="{ 'font-mono text-xs': l.task_id !== '' }" :title="l.task_id ? `Task ${l.task_id}` : undefined">
                {{ l.task_id ? shortTaskId(l.task_id) : 'lobby' }}
              </RouterLink>
            </td>
            <td class="whitespace-nowrap text-xs text-ink-muted">
              <RelativeTime v-if="sessionTime(l.started_ms, now) !== null" :value="l.started_ms" :now="now" />
              <template v-else>Unknown</template>
            </td>
            <td class="whitespace-nowrap text-xs text-ink-muted">
              <RelativeTime v-if="sessionTime(l.last_seen_ms, now) !== null" :value="l.last_seen_ms" :now="now" />
              <template v-else>Unknown</template>
            </td>
            <td class="whitespace-nowrap text-right">
              <button v-if="l.state === 'active'" type="button" class="btn-ghost btn-sm text-danger" :disabled="busy" @click="drain(l)">
                Drain<span class="sr-only"> lobby {{ l.id }}</span>
              </button>
            </td>
          </tr>
        </tbody>
      </table>
    </div>
  </section>
</template>

<script setup lang="ts">
// The lobby instances on the System page (GET /api/admin/lobbies): each one's
// address, state, sessions, task (to the lobby program's page) and times, and
// Drain once confirmed. Polled every 15 s; refresh() reads it again.
import { ref } from 'vue';
import { RouterLink } from 'vue-router';
import RelativeTime from './RelativeTime.vue';
import Badge from './ui/Badge.vue';
import LiveNotices from './ui/LiveNotices.vue';
import TableState from './ui/TableState.vue';
import { drainLobby, fetchLobbies } from '../api/admin';
import { confirmAction } from '../composables/useConfirm';
import { usePolling } from '../composables/usePolling';
import type { AdminLobby, LobbiesResponse } from '../types/admin';
import { lobbyStateLabel, lobbyUsageLink, sessionTime, shortTaskId } from '../utils/adminView';
import { countLabel } from '../utils/infraView';

withDefaults(defineProps<{ idBase?: string }>(), { idBase: 'lobbies' });

const POLL_MS = 15_000;

const data = ref<LobbiesResponse | null>(null);
const error = ref('');
const message = ref('');
const busy = ref(false);
const now = ref(Date.now());
// The error box shows a failed read (not an action's refusal).
let errorFromLoad = false;

async function drain(l: AdminLobby) {
  if (busy.value) return;
  if (!await confirmAction(`Drain lobby ${l.id}? It gets no new clients; its ${countLabel(l.sessions, 'session')} stay.`)) return;
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
