<template>
  <nav :aria-label="label">
    <ul class="space-y-1">
      <li v-for="item in items" :key="item.to">
        <RouterLink
          :to="item.to"
          :class="['workspace-nav-link', item.to === current ? 'bg-accent/10 text-accent ring-1 ring-inset ring-accent/20' : '']"
          :aria-current="item.to === current ? 'page' : undefined"
          @click="emit('navigate')"
        >
          {{ item.label }}
          <span v-if="item.badge" :class="[badgeClass(item.badge.tone), item.badge.class]" :title="item.badge.title">
            <span :aria-hidden="item.badge.textHidden ? 'true' : undefined">{{ item.badge.text }}</span><span v-if="item.badge.sr" class="sr-only">{{ item.badge.sr }}</span>
          </span>
        </RouterLink>
      </li>
    </ul>
  </nav>
</template>

<script lang="ts">
import type { Tone } from '../../utils/tone';

export interface SideNavItem {
  to: string;
  label: string;
  // A count or mark beside the label: its text, what a screen reader hears
  // after it, its tone, and a class a test or style can find it by.
  badge?: { text: string; sr?: string; tone: Tone; class?: string; title?: string; textHidden?: boolean };
}
</script>

<script setup lang="ts">
// A workspace's side nav: its sections, the current one marked, each with an
// optional badge (what needs an admin, releases awaiting approval).
import { RouterLink } from 'vue-router';
import { badgeClass } from '../../utils/tone';

defineProps<{ label: string; items: SideNavItem[]; current: string | null }>();
const emit = defineEmits<{ navigate: [] }>();
</script>
