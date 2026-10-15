<template>
  <article class="px-4 pt-24 pb-16 sm:px-6" :aria-labelledby="`${id}-title`">
    <div class="mx-auto max-w-3xl space-y-8">
      <header class="space-y-3">
        <p class="text-sm uppercase tracking-[0.3em] text-accent">NovaWorld</p>
        <h1 :id="`${id}-title`" class="text-4xl font-semibold text-ink">{{ title }}</h1>
        <nav aria-label="Legal pages" class="flex flex-wrap gap-4 text-sm">
          <RouterLink
            v-for="page in PAGES"
            :key="page.to"
            :to="page.to"
            class="transition"
            :class="route.path === page.to ? 'font-semibold text-ink' : 'text-accent hover:text-ink-bright'"
            :aria-current="route.path === page.to ? 'page' : undefined"
          >
            {{ page.label }}
          </RouterLink>
        </nav>
      </header>

      <!-- The placeholder banner, until the operator publishes the text. -->
      <p
        :id="`${id}-draft`"
        role="note"
        class="rounded-panel border-2 border-warn bg-warn/10 p-4 text-sm font-semibold text-warn"
      >
        Draft: the operator has not published these terms yet. Nothing on this page is in force; each section says
        what it will cover.
      </p>

      <div class="space-y-8">
        <slot />
      </div>
    </div>
  </article>
</template>

<script setup lang="ts">
// The legal pages' frame (/terms, /privacy, /refunds): the title, the links
// between the three, and the draft banner every page carries until the
// operator writes its text. Each page's sections are LegalSection blocks
// naming what the operator must fill in.
import { RouterLink, useRoute } from 'vue-router';

defineProps<{
  // The base of the page's ids.
  id: string;
  title: string;
}>();

const PAGES = [
  { to: '/terms', label: 'Terms of service' },
  { to: '/privacy', label: 'Privacy notice' },
  { to: '/refunds', label: 'Refund policy' },
];

const route = useRoute();
</script>
