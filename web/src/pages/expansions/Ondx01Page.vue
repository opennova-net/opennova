<template>
  <section class="mx-auto max-w-4xl px-6 pb-20">
    <header class="py-12 text-center">
      <p class="text-xs uppercase tracking-[0.3em] text-ink-muted">OpenNova curated expansion</p>
      <h1 class="mt-2 text-3xl font-semibold tracking-tight text-ink md:text-4xl">OpenNova Demo Mod — DFX2</h1>
      <p class="mt-3 text-base text-ink-muted md:text-lg">Feature breakdown coming soon.</p>
      <RouterLink
        to="/expansions"
        class="mt-6 inline-flex items-center gap-2 rounded-control border border-border bg-raised px-4 py-2 text-sm font-semibold text-accent transition hover:bg-hover"
      >
        ← Back to catalog
      </RouterLink>
    </header>

    <DetailContent :slug="slug" />
  </section>
</template>

<script setup lang="ts">
import DetailContent from './shared/DetailContent.vue';

const slug = 'ondx01';
</script>
