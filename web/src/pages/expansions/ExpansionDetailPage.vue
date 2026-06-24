<template>
  <section class="mx-auto max-w-4xl px-6 pb-20">
    <header class="py-12 text-center">
      <p class="text-xs uppercase tracking-[0.3em] text-ink-muted">OpenNova curated expansion</p>
      <RouterLink
        to="/expansions"
        class="mt-6 inline-flex items-center gap-2 rounded-control border border-border bg-raised px-4 py-2 text-sm font-semibold text-accent transition hover:bg-hover"
      >
        ← Back to catalog
      </RouterLink>
    </header>

    <DetailContent :key="slug" :slug="slug" />
  </section>
</template>

<script setup lang="ts">
import { computed } from 'vue';
import { useRoute } from 'vue-router';
import DetailContent from './shared/DetailContent.vue';

const route = useRoute();
// The route param is the expansion slug; DetailContent fetches the matching
// expansion from /api/expansions. Data-driven — no per-slug page needed.
const slug = computed(() => String(route.params.slug ?? ''));
</script>
