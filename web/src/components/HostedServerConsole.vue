<template>
  <section
    class="space-y-3 rounded-panel border border-dashed border-border-strong p-4"
    :aria-labelledby="headingId"
    data-seam="hosted-console"
  >
    <h3 :id="headingId" class="text-lg font-semibold text-ink">Server console</h3>
    <p class="text-sm text-ink-muted">
      A console for this server (its players with kick and ban, its chat, its mission rotation and its
      log) is coming in a later update.
    </p>
    <p v-if="server.listing" class="text-sm text-ink-muted">
      Until then, chat, renaming and player controls for
      <span class="font-medium text-ink">{{ server.listing.server_name || server.settings.game_name }}</span>
      work from
      <RouterLink to="/me/servers" class="text-accent hover:text-ink-bright">My servers</RouterLink>.
    </p>
    <p v-else class="text-sm text-ink-muted">
      Until then, chat, renaming and player controls work from
      <RouterLink to="/me/servers" class="text-accent hover:text-ink-bright">My servers</RouterLink>
      once the server is listed.
    </p>
  </section>
</template>

<script setup lang="ts">
// THE SEAM for the hosted server's remote-admin console (ADR 0053 d5; the
// plan's PR K). K adds the service's admin link and its owner routes for this
// server's console: the player list with punt and ban, the chat it reads, the
// missions the server has (MISSION AVAILABLE) and its console log. Their panes
// replace this placeholder, taking the same `server` prop; HostedServerStatus
// mounts it while the server is live, and the page's polling (HostedServerPage)
// keeps `server` current. Until then it points at My servers, whose
// ServerCommand controls already reach a hosted server's listing.
import { RouterLink } from 'vue-router';
import type { HostedServer } from '../types/hostedServer';

const props = defineProps<{
  server: HostedServer;
}>();

const headingId = `hosted-console-${props.server.id}`;
</script>
