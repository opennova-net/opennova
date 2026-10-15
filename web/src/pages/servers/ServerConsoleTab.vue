<template>
  <div class="space-y-4">
    <p v-if="server.state !== 'running'" class="rounded-panel border border-border bg-surface p-3 text-sm text-ink-muted">
      {{
        server.live
          ? 'The console opens once the server is running. Its log so far is below.'
          : 'The server has ended, so its console is closed. Its log is below.'
      }}
    </p>
    <!-- Keyed by the mode, so the console starts over when the server starts
         or stops running. -->
    <HostedServerConsole
      :key="`${server.id}-${mode}`"
      :target="{ kind: scope, id: server.id }"
      :mode="scope"
      :server-name="server.listing?.server_name || server.settings.game_name"
      :heading-level="2"
      :log-only="mode !== 'live'"
      :follow="mode !== 'ended'"
      @gone="emit('gone')"
      @unauthorized="emit('session-ended')"
    />
  </div>
</template>

<script setup lang="ts">
// A hosted server page's Console tab: the whole console while the server
// runs (the owner's commands, or the admin's reads and cycle), its console log
// alone while it starts or stops (read on the tick), and once it has ended
// (read once).
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
