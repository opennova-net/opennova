<template>
  <div class="space-y-8">
    <div class="space-y-4">
      <p v-if="mode !== 'live'" class="console-closed text-sm text-ink-muted">{{ mode === 'log' ? 'Opens once the server runs.' : 'Closed: the server has ended.' }}</p>
      <!-- Keyed by the mode, so the console starts over when the server starts
           or stops running. -->
      <HostedServerConsole
        :key="`${server.id}-${mode}`"
        :target="{ kind: scope, id: server.id }"
        :mode="scope"
        :server-name="server.listing?.server_name || server.settings.game_name"
        :novalogic-slots="server.novalogic?.player_slots ?? []"
        :heading-level="2"
        :log-only="mode !== 'live'"
        :follow="mode !== 'ended'"
        @gone="emit('gone')"
        @unauthorized="emit('session-ended')"
      />
    </div>
    <!-- The owner's pushes to its listing: its name and message, the
         mission's end, weather and time. -->
    <ServerCommands v-if="listing" :server="listing" id-base="console-commands" @changed="readListingSoon" @refused="onRefused" />
  </div>
</template>

<script setup lang="ts">
// A hosted server page's Console tab: the whole console while the server runs
// (the owner's commands, the admin's reads and cycle), its console log alone
// while it starts or stops (read on the tick), and once it has ended (read
// once). While it runs and is listed, its owner also gets ServerCommands
// through the listing (GET /api/me/hosts finds it by the hosted server's id).
import { computed, onUnmounted, ref, watch } from 'vue';
import HostedServerConsole from '../../components/HostedServerConsole.vue';
import ServerCommands from '../../components/ServerCommands.vue';
import { listMyServers, type HostError } from '../../api/myServers';
import type { HostedServer } from '../../types/hostedServer';
import type { MyServer } from '../../types/myServers';

const props = defineProps<{
  server: HostedServer;
  scope: 'owner' | 'admin';
  now: number;
}>();

const emit = defineEmits<{
  (e: 'changed', server: HostedServer | null): void;
  (e: 'gone'): void;
  (e: 'session-ended'): void;
}>();

const mode = computed<'live' | 'log' | 'ended'>(() => {
  if (props.server.state === 'running') return 'live';
  return props.server.live ? 'log' : 'ended';
});

// The server's listing among the account's, while it runs and is listed.
const listing = ref<MyServer | null>(null);
async function readListing() {
  if (props.scope !== 'owner' || mode.value !== 'live' || !props.server.listing) {
    listing.value = null;
    return;
  }
  const result = await listMyServers();
  if (result.ok) listing.value = result.value.find((h) => h.hosted_server_id === props.server.id) ?? null;
}
watch(() => [props.server.id, mode.value, props.server.listing?.rid] as const, () => void readListing(), { immediate: true });

let soonTimer: ReturnType<typeof setTimeout> | undefined;
function readListingSoon() {
  if (soonTimer) clearTimeout(soonTimer);
  soonTimer = setTimeout(() => void readListing(), 1500);
}
onUnmounted(() => {
  if (soonTimer) clearTimeout(soonTimer);
});

function onRefused(error: HostError) {
  if (error.code === 'unauthorized') emit('session-ended');
  else if (error.code === 'unknown_rid' || error.code === 'not_hosting') void readListing();
}
</script>
