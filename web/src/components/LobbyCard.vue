<template>
  <div class="rounded-panel border border-border bg-panel p-5">
    <div class="flex items-center justify-between gap-3">
      <p class="min-w-0 truncate text-lg font-semibold text-ink">{{ host.serverName || 'Unnamed server' }}</p>
      <span class="shrink-0 rounded-control bg-hover px-3 py-1 text-xs font-semibold text-accent">
        {{ host.players }} / {{ host.maxPlayers || 0 }}
      </span>
    </div>
    <p v-if="host.mission" class="mt-2 text-sm text-ink">
      {{ host.mission }}<span v-if="host.gameType" class="text-ink-muted"> ({{ host.gameType }})</span>
    </p>
    <p v-if="host.expansion || host.locked" class="mt-2 flex flex-wrap gap-2 text-xs">
      <span v-if="host.expansion" class="rounded-control bg-accent/15 px-2 py-0.5 text-accent">Needs {{ host.expansion }}</span>
      <span v-if="host.locked" class="rounded-control bg-hover px-2 py-0.5 text-ink-muted">Password</span>
    </p>
    <p v-if="host.hostIp" class="mt-2 font-mono text-xs text-ink-muted">{{ host.hostIp }}:{{ host.hostPort }}</p>
  </div>
</template>

<script setup lang="ts">
import type { LobbyHost } from '../types/lobby';

interface Props {
  host: LobbyHost;
}

defineProps<Props>();
</script>
