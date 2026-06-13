<template>
  <div class="rounded-panel border border-border bg-panel p-5">
    <div class="flex items-center justify-between">
      <p class="text-lg font-semibold text-ink">{{ host.serverName || 'Unnamed Server' }}</p>
      <span class="rounded-control bg-hover px-3 py-1 text-xs font-semibold text-accent">
        {{ host.players }} / {{ host.maxPlayers || 0 }}
      </span>
    </div>
    <p class="mt-2 text-sm text-ink-muted">{{ host.region || 'Unknown region' }}</p>
    <p class="mt-1 text-xs font-mono text-ink-muted" v-if="host.hostIp">{{ host.hostIp }}:{{ host.hostPort }}</p>
    <p class="mt-4 text-sm text-ink-muted" v-if="host.message">{{ host.message }}</p>
  </div>
</template>

<script setup lang="ts">
import type { LobbyHost } from '../types/lobby';

interface Props {
  host: LobbyHost;
}

defineProps<Props>();
</script>
