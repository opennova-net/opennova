<template>
  <RouterLink :to="`/admin/products/${encodeURIComponent(String(productRef))}`" class="product-link entity-link">{{ name }}</RouterLink>
</template>

<script setup lang="ts">
// A product wherever an admin page names one, to its page: by its project key,
// or its id when it has none.
import { RouterLink } from 'vue-router';

defineProps<{ productRef: string | number; name: string }>();
</script>
