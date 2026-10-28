<template>
  <section class="px-4 py-10 sm:px-6 sm:py-14" :aria-labelledby="`${id}-title`">
    <div class="card mx-auto max-w-md space-y-6 p-6 sm:p-8">
      <header class="space-y-1.5">
        <h1 :id="`${id}-title`" class="page-heading">{{ title }}</h1>
        <p v-if="$slots.subtitle" class="text-sm text-ink-muted"><slot name="subtitle" /></p>
      </header>
      <slot />
      <div v-if="$slots.footer" class="space-y-1 text-sm text-ink-muted"><slot name="footer" /></div>
    </div>
  </section>
</template>

<script setup lang="ts">
// The log in, register and password pages' one shell: a card with the title,
// an optional line under it, the form, and the links below.
defineProps<{ title: string; id: string }>();
</script>
