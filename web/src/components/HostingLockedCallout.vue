<template>
  <section class="space-y-3 rounded-panel border border-accent/40 bg-accent/5 p-4 sm:p-6" :aria-labelledby="`${id}-heading`">
    <h2 :id="`${id}-heading`" class="text-lg font-semibold text-ink">Host your own server on NovaWorld</h2>
    <p class="text-sm text-ink-muted">
      A support tier lets you run hosted servers: NovaWorld starts them on its machines, lists them in the
      game's server browser and keeps them running until you stop them. Tiers are monthly and support the
      project; hosted servers are pre-1.0 and experimental.
    </p>
    <RouterLink
      to="/me/support"
      class="inline-block rounded-control bg-accent px-5 py-2.5 text-sm font-semibold text-on-accent transition hover:bg-accent/90"
    >
      See the support tiers
    </RouterLink>
  </section>
</template>

<script setup lang="ts">
// The Servers page's hosting area while the account's entitlement is 0
// servers: what a support tier buys, and the way to the tiers.
import { RouterLink } from 'vue-router';

defineProps<{
  // The base of the section's ids.
  id: string;
}>();
</script>
