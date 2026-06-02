<template>
  <section class="mx-auto max-w-4xl px-6 pb-20">
    <header class="py-12 text-center">
      <p class="text-xs uppercase tracking-[0.3em] text-slate-500">OpenNova curated expansion</p>
      <h1 class="mt-2 text-3xl font-semibold tracking-tight text-white md:text-4xl">Tactical Assault Coalition</h1>
      <p class="mt-3 text-base text-slate-300 md:text-lg">Feature breakdown coming soon.</p>
      <RouterLink
        to="/expansions"
        class="mt-6 inline-flex items-center gap-2 rounded-lg border border-white/10 bg-white/5 px-4 py-2 text-sm font-semibold text-brand-100 transition hover:bg-white/10"
      >
        ← Back to catalog
      </RouterLink>
    </header>

    <DetailContent :slug="slug" />
  </section>
</template>

<script setup lang="ts">
import DetailContent from './shared/DetailContent.vue';

const slug = 'revx02';
</script>
