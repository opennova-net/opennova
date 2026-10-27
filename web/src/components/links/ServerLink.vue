<template>
  <span class="server-link inline-flex min-w-0 flex-wrap items-baseline gap-x-2">
    <RouterLink v-if="to" :to="to" class="entity-link break-words font-medium"><GameText :text="serverName(name)" /></RouterLink>
    <span v-else class="break-words font-medium"><GameText :text="serverName(name)" /></span>
    <span v-if="game" class="text-xs text-ink-muted">{{ game }}</span>
  </span>
</template>

<script setup lang="ts">
// A server wherever a page names one: its name in the game's markup, linked
// to its page (a hosted server's admin or owner page, else its public
// listing), and the game it runs beside it when the page says.
import { computed } from 'vue';
import { RouterLink } from 'vue-router';
import GameText from '../GameText.vue';
import { serverName } from '../../utils/games';

const props = withDefaults(
  defineProps<{
    name: string | null | undefined;
    // The hosted server's id; null for a server a player hosts.
    id?: number | null;
    // Its listing's id, for the public page of one with no hosted id.
    rid?: number | null;
    scope?: 'admin' | 'owner' | 'public';
    game?: string;
  }>(),
  { id: null, rid: null, scope: 'admin', game: '' },
);

const to = computed(() => {
  if (props.id != null && props.scope !== 'public') return props.scope === 'admin' ? `/admin/servers/${props.id}` : `/me/servers/${props.id}`;
  if (props.rid != null) return `/lobby/${props.rid}`;
  return null;
});
</script>
