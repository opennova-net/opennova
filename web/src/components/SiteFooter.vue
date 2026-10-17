<template>
  <footer class="border-t border-border bg-panel/40 py-10 text-sm">
    <div :class="[wide ? 'page-wide' : 'page', 'grid grid-cols-3 gap-6 lg:grid-cols-4']">
      <div class="col-span-3 space-y-2 lg:col-span-1">
        <p class="font-semibold text-ink">Open<span class="text-accent">Nova</span></p>
        <p class="text-ink-muted">
          A free, open-source game on NovaLogic's engine, rebuilt in the open, that also plays the Joint Operations you own. Pre-1.0 and experimental.
        </p>
      </div>
      <nav aria-label="Project" class="space-y-2">
        <p class="font-semibold text-ink">Project</p>
        <ul class="space-y-1.5 text-ink-muted">
          <li><a :href="RELEASES_URL" target="_blank" rel="noopener" class="transition hover:text-ink-bright">Downloads</a></li>
          <li><a :href="GITHUB_URL" target="_blank" rel="noopener" class="transition hover:text-ink-bright">GitHub</a></li>
        </ul>
      </nav>
      <nav aria-label="NovaWorld" class="space-y-2">
        <p class="font-semibold text-ink">NovaWorld</p>
        <ul class="space-y-1.5 text-ink-muted">
          <li><RouterLink to="/lobby" class="transition hover:text-ink-bright">Lobbies</RouterLink></li>
          <li><RouterLink to="/support" class="transition hover:text-ink-bright">Support tiers</RouterLink></li>
          <li><RouterLink to="/register" class="transition hover:text-ink-bright">Create an account</RouterLink></li>
        </ul>
      </nav>
      <nav aria-label="Legal" class="space-y-2">
        <p class="font-semibold text-ink">Legal</p>
        <ul class="space-y-1.5 text-ink-muted">
          <li><RouterLink to="/terms" class="transition hover:text-ink-bright">Terms</RouterLink></li>
          <li><RouterLink to="/privacy" class="transition hover:text-ink-bright">Privacy</RouterLink></li>
          <li><RouterLink to="/refunds" class="transition hover:text-ink-bright">Refunds</RouterLink></li>
        </ul>
      </nav>
    </div>
    <p :class="[wide ? 'page-wide' : 'page', 'mt-8 text-xs text-ink-muted']">
      Fan-run preservation project, not endorsed by THQ Nordic; original rights remain with their owners.
    </p>
  </footer>
</template>

<script setup lang="ts">
import { computed } from 'vue';
import { RouterLink, useRoute } from 'vue-router';
import { GITHUB_URL, RELEASES_URL } from '../utils/links';

// The admin pages are wide; the footer lines up with them.
const route = useRoute();
const wide = computed(() => route.matched.some((record) => record.meta.wide));
</script>
