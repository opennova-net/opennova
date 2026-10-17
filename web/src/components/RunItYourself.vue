<template>
  <aside class="card p-6" :aria-labelledby="`${id}-heading`">
    <h3 :id="`${id}-heading`" class="text-lg font-semibold text-ink-bright">Rather not pay? Run it yourself.</h3>
    <p class="mt-2 text-ink-muted">
      Everything is open source, so you can run servers yourself, free. opennova-serve, in the tools download, lists
      itself on our NovaWorld with <code class="font-mono text-sm text-ink">--master-host {{ NOVAWORLD_GATE_HOST }}</code>,
      or serves your LAN without it. You can also host from inside the game, or run the whole NovaWorld service on
      machines of your own.
    </p>
    <p class="mt-3 flex flex-wrap gap-x-6 gap-y-2 text-sm">
      <a :href="SERVE_README_URL" target="_blank" rel="noopener" class="link">Running opennova-serve</a>
      <a :href="DEPLOY_URL" target="_blank" rel="noopener" class="link">Running NovaWorld</a>
    </p>
  </aside>
</template>

<script setup lang="ts">
// The free way to a server: opennova-serve or the whole service, run by the
// player, beside the support tiers (the home page and the Support page).
import { DEPLOY_URL, NOVAWORLD_GATE_HOST, SERVE_README_URL } from '../utils/links';

withDefaults(
  defineProps<{
    // The base of the section's ids.
    id?: string;
  }>(),
  { id: 'run-it-yourself' }
);
</script>
