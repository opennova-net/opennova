<template>
  <section class="page py-24 text-center" aria-labelledby="not-found-title">
    <p class="eyebrow">404</p>
    <h1 id="not-found-title" class="mt-3 text-3xl font-semibold text-ink-bright">There is no page here</h1>
    <p class="mt-3 text-ink-muted">The link may be old, or the address mistyped.</p>
    <div class="mt-8 flex flex-wrap justify-center gap-3">
      <RouterLink to="/" class="btn-primary">Go home</RouterLink>
      <RouterLink to="/lobby" class="btn-secondary">View lobbies</RouterLink>
    </div>
  </section>
</template>

<script setup lang="ts">
import { RouterLink } from 'vue-router';
</script>
