<template>
  <nav v-if="place && place.area" aria-label="Breadcrumb" class="admin-crumbs text-sm">
    <ol class="flex flex-wrap items-center gap-x-2 gap-y-1 text-ink-muted">
      <li class="font-medium">{{ place.area.label }}</li>
      <li v-if="!onSection">
        <span aria-hidden="true" class="mr-2">/</span>
        <RouterLink :to="place.section.to" class="text-accent hover:text-ink-bright">{{ place.section.label }}</RouterLink>
      </li>
      <li v-for="crumb in trail" :key="crumb.label">
        <span aria-hidden="true" class="mr-2">/</span>
        <RouterLink v-if="crumb.to" :to="crumb.to" class="text-accent hover:text-ink-bright">{{ crumb.label }}</RouterLink>
        <span v-else>{{ crumb.label }}</span>
      </li>
    </ol>
  </nav>
</template>

<script setup lang="ts">
// Where an admin page sits (utils/adminNav.ts): its area in words, then its
// section's page as a link when this is a page under it (a user's, a fleet
// server's), then any further steps the page names (`trail`: a job's kind). The
// Overview has none.
import { computed } from 'vue';
import { RouterLink, useRoute } from 'vue-router';
import { placeOf, type Crumb } from '../utils/adminNav';

withDefaults(defineProps<{ trail?: Crumb[] }>(), { trail: () => [] });

// No router (a page mounted alone in a test): no breadcrumb.
const route = useRoute() as ReturnType<typeof useRoute> | undefined;
const path = computed(() => route?.path ?? '');
const place = computed(() => placeOf(path.value));
const onSection = computed(() => place.value !== null && path.value === place.value.section.to);
</script>
