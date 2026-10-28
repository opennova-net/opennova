<template>
  <RouterLink v-if="id != null" :to="`/admin/players/${id}`" class="player-link entity-link">{{ name }}</RouterLink>
  <span v-else class="player-link">{{ name || 'Deleted account' }}</span>
</template>

<script setup lang="ts">
// A player wherever an admin page names one: the account's page, or the name
// alone for a player with no account here (one NovaLogic's NovaWorld sent).
import { RouterLink } from 'vue-router';

defineProps<{ id?: number | null; name: string }>();
</script>
