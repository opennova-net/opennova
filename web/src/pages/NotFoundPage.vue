<template>
  <section class="page space-y-6 py-16">
    <PageHeader title="Page not found" />
    <p class="text-ink-muted">The link may be old, or the address mistyped.</p>
    <div class="flex flex-wrap gap-3">
      <RouterLink to="/" class="btn-primary">Go home</RouterLink>
      <RouterLink to="/lobby" class="btn-secondary">Servers</RouterLink>
    </div>
  </section>
</template>

<script setup lang="ts">
// Any path the site has no page for.
import { RouterLink } from 'vue-router';
import PageHeader from '../components/ui/PageHeader.vue';
</script>
