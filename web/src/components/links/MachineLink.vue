<template>
  <RouterLink :to="`/admin/machines/${id}`" class="machine-link entity-link whitespace-nowrap">{{ label || `Machine ${id}` }}</RouterLink>
</template>

<script setup lang="ts">
// A fleet machine wherever a page names one, to its page.
import { RouterLink } from 'vue-router';

defineProps<{ id: number | string; label?: string }>();
</script>
