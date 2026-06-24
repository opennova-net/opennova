<template>
  <div class="space-y-10">
    <header class="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
      <div>
        <h1 class="text-3xl font-semibold text-ink">Connections</h1>
        <p class="text-sm text-ink-muted">
          Live sessions on the server.
          <span v-if="!loading">{{ count }} active · heartbeat timeout {{ heartbeatTimeoutMs }} ms</span>
        </p>
      </div>
      <button
        type="button"
        class="rounded-control border border-border bg-raised px-4 py-2 text-sm font-medium text-ink transition hover:bg-hover disabled:cursor-not-allowed disabled:opacity-40"
        :disabled="loading"
        @click="load"
      >
        Refresh
      </button>
    </header>

    <div v-if="error" class="rounded-panel border border-danger/40 bg-danger/10 p-4 text-sm text-danger">
      {{ error }}
    </div>

    <section class="space-y-4">
      <div class="overflow-x-auto rounded-panel border border-border bg-panel">
        <table class="min-w-full divide-y divide-border">
          <thead>
            <tr class="bg-raised text-left text-xs font-semibold uppercase tracking-wider text-ink-muted">
              <th class="px-4 py-3">ID</th>
              <th class="px-4 py-3">State</th>
              <th class="px-4 py-3">PN</th>
              <th class="px-4 py-3">Identity</th>
              <th class="px-4 py-3">Address</th>
              <th class="px-4 py-3">Created</th>
              <th class="px-4 py-3">Last seen</th>
            </tr>
          </thead>
          <tbody class="divide-y divide-border text-sm text-ink">
            <tr v-if="loading">
              <td colspan="7" class="px-4 py-6 text-center text-ink-muted">Loading…</td>
            </tr>
            <tr v-else-if="connections.length === 0">
              <td colspan="7" class="px-4 py-6 text-center text-ink-muted">No active connections.</td>
            </tr>
            <tr v-for="c in connections" :key="c.id" class="hover:bg-hover">
              <td class="px-4 py-3 font-mono text-xs">{{ c.id }}</td>
              <td class="px-4 py-3">{{ c.state }}</td>
              <td class="px-4 py-3 font-mono text-xs">{{ c.pn }}</td>
              <td class="px-4 py-3">{{ c.identity || '—' }}</td>
              <td class="px-4 py-3 font-mono text-xs">{{ c.addr }}</td>
              <td class="px-4 py-3 text-xs text-ink-muted">{{ formatMs(c.created_ms) }}</td>
              <td class="px-4 py-3 text-xs text-ink-muted">{{ formatMs(c.last_seen_ms) }}</td>
            </tr>
          </tbody>
        </table>
      </div>
    </section>
  </div>
</template>

<script setup lang="ts">
import { isAxiosError } from 'axios';
import { onMounted, ref } from 'vue';
import { fetchConnections } from '../../api/admin';
import type { AdminConnection } from '../../types/admin';

const loading = ref(true);
const error = ref<string | null>(null);
const connections = ref<AdminConnection[]>([]);
const count = ref(0);
const heartbeatTimeoutMs = ref(0);

function extractError(err: unknown): string {
  if (isAxiosError(err)) {
    const data = err.response?.data as Record<string, any> | undefined;
    if (typeof data?.message === 'string') return data.message;
    if (typeof data?.error === 'string') return data.error;
    return err.response?.statusText || 'Request failed';
  }
  if (err instanceof Error) return err.message;
  return 'Unexpected error';
}

// Server timestamps are epoch milliseconds. Render them locally; 0/absent → dash.
function formatMs(ms?: number): string {
  if (!ms) return '—';
  const date = new Date(ms);
  return Number.isNaN(date.getTime()) ? String(ms) : date.toLocaleString();
}

async function load() {
  loading.value = true;
  error.value = null;
  try {
    const data = await fetchConnections();
    connections.value = data.connections ?? [];
    count.value = data.count ?? connections.value.length;
    heartbeatTimeoutMs.value = data.heartbeat_timeout_ms ?? 0;
  } catch (err) {
    error.value = extractError(err);
  } finally {
    loading.value = false;
  }
}

onMounted(load);
</script>
