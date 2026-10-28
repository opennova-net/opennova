<template>
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
</template>

<script setup lang="ts">
// A hosted server page's Console tab: the whole console while the server runs
// (the owner's commands, the admin's reads and cycle), its console log alone
// while it starts or stops (read on the tick), and once it has ended (read
// once).
import { computed } from 'vue';
import HostedServerConsole from '../../components/HostedServerConsole.vue';
import type { HostedServer } from '../../types/hostedServer';

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
</script>
