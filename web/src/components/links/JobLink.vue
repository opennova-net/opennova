<template>
  <RouterLink :to="`/admin/jobs/${id}`" class="job-link entity-link whitespace-nowrap">{{ label || `Job ${id}` }}</RouterLink>
</template>

<script setup lang="ts">
// A background job wherever a page names one, to its page.
import { RouterLink } from 'vue-router';

defineProps<{ id: number; label?: string }>();
</script>
