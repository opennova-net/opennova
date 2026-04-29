<template>
  <div class="rounded-3xl border border-white/5 bg-white/5 p-5 shadow-xl">
    <div class="flex items-center justify-between">
      <p class="text-lg font-semibold text-white">{{ host.serverName || 'Unnamed Server' }}</p>
      <span class="rounded-full bg-white/10 px-3 py-1 text-xs font-semibold text-brand-100">
        {{ host.players }} / {{ host.maxPlayers || 0 }}
      </span>
    </div>
    <p class="mt-2 text-sm text-slate-300">{{ host.region || 'Unknown region' }}</p>
    <p class="mt-4 text-sm text-slate-400" v-if="host.message">{{ host.message }}</p>
  </div>
</template>

<script setup lang="ts">
import type { LobbyHost } from '../types/lobby';

interface Props {
  host: LobbyHost;
}

defineProps<Props>();
</script>
