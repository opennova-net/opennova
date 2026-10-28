<template>
  <footer class="border-t border-border bg-panel/40 py-8 text-sm">
    <div :class="[wide ? 'page-wide' : 'page', 'flex flex-col gap-6 sm:flex-row sm:items-start sm:justify-between']">
      <div class="space-y-1">
        <p class="font-semibold text-ink">Open<span class="text-accent">Nova</span></p>
        <p class="site-status text-ink-muted">Pre-1.0 and experimental.</p>
      </div>
      <nav aria-label="Site" class="grid grid-flow-col grid-rows-3 gap-x-8 gap-y-1.5 text-ink-muted sm:gap-x-12">
        <RouterLink to="/play" class="transition hover:text-ink-bright">Play</RouterLink>
        <RouterLink to="/downloads" class="transition hover:text-ink-bright">Downloads</RouterLink>
        <a :href="GITHUB_URL" target="_blank" rel="noopener" class="transition hover:text-ink-bright">GitHub</a>
        <RouterLink to="/lobby" class="transition hover:text-ink-bright">Servers</RouterLink>
        <RouterLink to="/hosting" class="transition hover:text-ink-bright">Hosting</RouterLink>
        <a :href="ISSUES_URL" target="_blank" rel="noopener" class="transition hover:text-ink-bright">Report a problem</a>
        <RouterLink to="/terms" class="transition hover:text-ink-bright">Terms</RouterLink>
        <RouterLink to="/privacy" class="transition hover:text-ink-bright">Privacy</RouterLink>
        <RouterLink to="/refunds" class="transition hover:text-ink-bright">Refunds</RouterLink>
      </nav>
    </div>
    <p :class="[wide ? 'page-wide' : 'page', 'mt-6 text-xs text-ink-muted']">
      Fan-run preservation project, not endorsed by THQ Nordic; original rights remain with their owners.
    </p>
  </footer>
</template>

<script setup lang="ts">
// Every page's foot: the project's status, the site's pages, GitHub and the
// legal pages.
import { computed } from 'vue';
import { RouterLink, useRoute } from 'vue-router';
import { GITHUB_URL, ISSUES_URL } from '../utils/links';

// The admin pages are wide; the footer lines up with them.
const route = useRoute();
const wide = computed(() => route.matched.some((record) => record.meta.wide));
</script>
