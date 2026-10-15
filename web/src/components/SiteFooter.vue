<template>
  <footer class="border-t border-border bg-surface/80 py-8">
    <div class="mx-auto flex max-w-6xl flex-col gap-3 px-4 text-[0.65rem] text-ink-muted sm:px-6">
      <p class="tracking-[0.18em] uppercase">
        Fan-run preservation project, not endorsed by THQ Nordic; original rights remain with their owners.
      </p>
      <nav aria-label="Site" class="flex flex-wrap items-center gap-x-4 gap-y-2 tracking-[0.18em] uppercase">
        <RouterLink to="/support" class="transition hover:text-ink-bright">Support tiers</RouterLink>
        <RouterLink to="/terms" class="transition hover:text-ink-bright">Terms</RouterLink>
        <RouterLink to="/privacy" class="transition hover:text-ink-bright">Privacy</RouterLink>
        <RouterLink to="/refunds" class="transition hover:text-ink-bright">Refunds</RouterLink>
        <a href="https://github.com/opennova-net/" target="_blank" rel="noopener" class="transition hover:text-ink-bright">GitHub</a>
        <a href="https://discord.gg/comingsoon" target="_blank" rel="noopener" class="transition hover:text-ink-bright">Discord</a>
      </nav>
    </div>
  </footer>
</template>

<script setup lang="ts">
import { RouterLink } from 'vue-router';
</script>
