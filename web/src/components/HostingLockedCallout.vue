<template>
  <section class="space-y-3 rounded-panel border border-accent/40 bg-accent/5 p-4 sm:p-6" :aria-labelledby="`${id}-heading`">
    <h2 :id="`${id}-heading`" class="text-lg font-semibold text-ink">Host your own server on NovaWorld</h2>
    <p class="text-sm text-ink-muted">
      A monthly support tier lets you run hosted servers and funds the project. A dedicated machine runs one more on a
      machine of its own, with or without a tier. An admin can also grant your account hosted servers, free of charge.
      Hosted servers run OpenNova's own server, opennova-serve, not the original game, and are pre-1.0 and experimental.
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
// servers: the three ways to host (a support tier, a dedicated machine, an
// admin's grant) and the way to the support tier page, which sells the first
// two.
import { RouterLink } from 'vue-router';

defineProps<{
  // The base of the section's ids.
  id: string;
}>();
</script>
