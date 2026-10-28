<template>
  <div class="space-y-6">
    <PageHeader title="Servers" :tabs="MY_SERVERS_TABS" />

    <LiveNotices :error="error" />

    <p v-if="data === null && !error" class="text-sm text-ink-muted">Loading…</p>
    <p v-else-if="data && data.servers.length === 0" class="servers-stats-empty text-sm text-ink-muted">No servers yet.</p>
    <StatsDashboard
      v-else
      base-id="servers-stats"
      heading="All servers"
      variant="aggregate"
      :follow="anyLive"
      :load="fetchAccountStats"
      @session-ended="onSessionEnded"
    />
  </div>
</template>

<script setup lang="ts">
// /me/servers/stats: the account's servers together over time (GET
// /api/me/servers/stats), following them while any is live (GET
// /api/me/servers, on the hosted cadence).
import { computed, ref } from 'vue';
import { useRoute, useRouter } from 'vue-router';
import StatsDashboard from '../../components/StatsDashboard.vue';
import LiveNotices from '../../components/ui/LiveNotices.vue';
import PageHeader from '../../components/ui/PageHeader.vue';
import { fetchMyServers } from '../../api/hostedServer';
import { fetchAccountStats } from '../../api/hostedStats';
import { hostedCadence, usePolling } from '../../composables/usePolling';
import { useSession } from '../../composables/useSession';
import type { HostedServersResponse } from '../../types/hostedServer';
import { MY_SERVERS_TABS } from '../../utils/accountNav';

const { refresh: refreshSession } = useSession();
const route = useRoute();
const router = useRouter();

const data = ref<HostedServersResponse | null>(null);
const error = ref('');

const anyLive = computed(() => data.value?.servers.some((s) => s.live) ?? false);

async function onSessionEnded() {
  await refreshSession();
  await router.replace({ path: '/login', query: { redirect: route.fullPath } });
}

const cadence = hostedCadence();

usePolling({
  async read(signal, background) {
    const result = await fetchMyServers(signal);
    if (signal.aborted) return false;
    if (result.ok) {
      data.value = result.value;
      error.value = '';
      return true;
    }
    if (result.error.code === 'unauthorized') {
      await onSessionEnded();
      return false;
    }
    if (!background || data.value === null) error.value = result.error.message;
    return false;
  },
  next: () => cadence(data.value?.servers ?? []),
});
</script>
