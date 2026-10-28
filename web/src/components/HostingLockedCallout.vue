<template>
  <Notice :id="id" tone="info" class="hosting-locked flex flex-wrap items-center justify-between gap-3">
    <span>Host your own server on NovaWorld with a tier or a dedicated machine.</span>
    <RouterLink to="/hosting" class="btn-primary btn-sm">See hosting</RouterLink>
  </Notice>
</template>

<script setup lang="ts">
// The Servers page's hosting area while the account may run no server: one
// line and the way to the Hosting page.
import { RouterLink } from 'vue-router';
import Notice from './ui/Notice.vue';

defineProps<{ id: string }>();
</script>
